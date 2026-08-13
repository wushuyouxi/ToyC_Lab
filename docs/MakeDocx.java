// 简易 docx 生成器:把带标记的文本转换为 Word 文档(.docx)。
// 标记格式(每行一种):
//   !PAGE      分页符
//   !TOC       Word 目录域(在 Word 中按 F9 更新)
//   # / ## / ###   一级/二级/三级标题
//   C: 居中段落   CB: 居中加粗(14pt)   CS: 居中大标题(18pt)
//   - 项目符号段落
//   连续的 | 行   表格(首行为表头)
//   P: 正文段落
// 用法: javac MakeDocx.java && java MakeDocx 内容.txt 输出.docx
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

public class MakeDocx {
  static StringBuilder body = new StringBuilder();

  static String esc(String s) {
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
  }

  static void para(String text, boolean bold, int szHalfPt, boolean center, int outlineLvl) {
    StringBuilder p = new StringBuilder("<w:p><w:pPr>");
    if (center) p.append("<w:jc w:val=\"center\"/>");
    if (outlineLvl >= 0) p.append("<w:outlineLvl w:val=\"").append(outlineLvl).append("\"/>");
    p.append("<w:spacing w:before=\"60\" w:after=\"60\" w:line=\"360\" w:lineRule=\"auto\"/></w:pPr>");
    p.append("<w:r><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:eastAsia=\"宋体\"/>");
    if (bold) p.append("<w:b/>");
    p.append("<w:sz w:val=\"").append(szHalfPt).append("\"/><w:szCs w:val=\"").append(szHalfPt).append("\"/>");
    p.append("</w:rPr><w:t xml:space=\"preserve\">").append(esc(text)).append("</w:t></w:r></w:p>");
    body.append(p);
  }

  static void bullet(String text) {
    StringBuilder p = new StringBuilder("<w:p><w:pPr>");
    p.append("<w:ind w:left=\"420\" w:hanging=\"210\"/>");
    p.append("<w:spacing w:before=\"40\" w:after=\"40\" w:line=\"360\" w:lineRule=\"auto\"/></w:pPr>");
    p.append("<w:r><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:eastAsia=\"宋体\"/>");
    p.append("<w:sz w:val=\"24\"/></w:rPr><w:t xml:space=\"preserve\">• ").append(esc(text)).append("</w:t></w:r></w:p>");
    body.append(p);
  }

  static void pageBreak() {
    body.append("<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>");
  }

  static void tocField() {
    body.append("<w:p><w:r><w:fldChar w:fldCharType=\"begin\" w:dirty=\"true\"/></w:r>");
    body.append("<w:r><w:instrText xml:space=\"preserve\"> TOC \\o \"1-3\" \\h \\z \\u </w:instrText></w:r>");
    body.append("<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>");
    body.append("<w:r><w:rPr><w:rFonts w:eastAsia=\"宋体\"/></w:rPr>");
    body.append("<w:t>（目录：在 Word 中全选后按 F9，或右键选择“更新域”自动生成）</w:t></w:r>");
    body.append("<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>");
  }

  static void cell(StringBuilder sb, String text, boolean bold) {
    sb.append("<w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/><w:vAlign w:val=\"center\"/></w:tcPr>");
    sb.append("<w:p><w:pPr><w:spacing w:before=\"20\" w:after=\"20\"/></w:pPr>");
    sb.append("<w:r><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:eastAsia=\"宋体\"/>");
    if (bold) sb.append("<w:b/>");
    sb.append("<w:sz w:val=\"22\"/></w:rPr><w:t xml:space=\"preserve\">").append(esc(text)).append("</w:t></w:r></w:p></w:tc>");
  }

  static void table(List<String[]> rows) {
    StringBuilder t = new StringBuilder("<w:tbl><w:tblPr><w:tblBorders>");
    for (String side : new String[]{"top", "left", "bottom", "right", "insideH", "insideV"})
      t.append("<w:").append(side).append(" w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"000000\"/>");
    t.append("</w:tblBorders><w:tblW w:w=\"0\" w:type=\"auto\"/><w:jc w:val=\"center\"/></w:tblPr><w:tblGrid>");
    int cols = rows.get(0).length;
    for (int i = 0; i < cols; ++i) t.append("<w:gridCol w:w=\"").append(9000 / cols).append("\"/>");
    t.append("</w:tblGrid>");
    for (int r = 0; r < rows.size(); ++r) {
      t.append("<w:tr>");
      for (int c = 0; c < cols; ++c) cell(t, rows.get(r)[c], r == 0);
      t.append("</w:tr>");
    }
    t.append("</w:tbl>");
    body.append(t);
  }

  static void put(String name, String content) throws IOException {
    ZipOutputStream zip = null;  // 在 main 中统一写入
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 2) { System.err.println("usage: MakeDocx in.txt out.docx"); System.exit(1); }
    List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);

    int i = 0;
    while (i < lines.size()) {
      String line = lines.get(i);
      if (line.startsWith("|")) {  // 表格:收集连续 | 行
        List<String[]> rows = new ArrayList<>();
        while (i < lines.size() && lines.get(i).startsWith("|")) {
          String[] cells = lines.get(i).substring(1).split("\\|", -1);
          for (int c = 0; c < cells.length; ++c) cells[c] = cells[c].trim();
          rows.add(cells);
          ++i;
        }
        table(rows);
        continue;
      }
      if (line.startsWith("!PAGE")) { pageBreak(); ++i; continue; }
      if (line.startsWith("!TOC")) { tocField(); ++i; continue; }
      if (line.startsWith("### ")) { para(line.substring(4), true, 24, false, 2); ++i; continue; }
      if (line.startsWith("## ")) { para(line.substring(3), true, 28, false, 1); ++i; continue; }
      if (line.startsWith("# ")) { para(line.substring(2), true, 32, false, 0); ++i; continue; }
      if (line.startsWith("CS: ")) { para(line.substring(4), true, 36, true, -1); ++i; continue; }
      if (line.startsWith("CB: ")) { para(line.substring(4), true, 28, true, -1); ++i; continue; }
      if (line.startsWith("C: ")) { para(line.substring(3), false, 24, true, -1); ++i; continue; }
      if (line.startsWith("- ")) { bullet(line.substring(2)); ++i; continue; }
      if (line.startsWith("P: ")) { para(line.substring(3), false, 24, false, -1); ++i; continue; }
      para(line, false, 24, false, -1);
      ++i;
    }

    String documentXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        + "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        + "<w:body>" + body
        + "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"851\" w:footer=\"992\" w:gutter=\"0\"/></w:sectPr>"
        + "</w:body></w:document>";

    String contentTypes = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        + "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        + "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        + "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        + "</Types>";

    String rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        + "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        + "</Relationships>";

    try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(args[1]))) {
      addEntry(zip, "[Content_Types].xml", contentTypes);
      addEntry(zip, "_rels/.rels", rels);
      addEntry(zip, "word/document.xml", documentXml);
    }
    System.out.println("written: " + args[1]);
  }

  static void addEntry(ZipOutputStream zip, String name, String content) throws IOException {
    zip.putNextEntry(new ZipEntry(name));
    zip.write(content.getBytes(StandardCharsets.UTF_8));
    zip.closeEntry();
  }
}
