"""Writes src/generated/JsUnicodeData.sa: the Unicode tables of the JavaScript
front-end, taken from what Node.js itself answers, so that the two agree.

Run from compiler/:  python src/tools/gen_js_unicode.py [node]

Node has no API for canonical combining classes: those come from Python's
unicodedata, and the result is only used to order combining marks.
"""
import json, os, subprocess, sys, tempfile, unicodedata

NODE = sys.argv[1] if len(sys.argv) > 1 else "node"
OUT = "src/generated/JsUnicodeData.sa"

GC = """L Letter LC Cased_Letter Lu Uppercase_Letter Ll Lowercase_Letter Lt Titlecase_Letter Lm Modifier_Letter Lo Other_Letter
M Mark Combining_Mark Mn Nonspacing_Mark Mc Spacing_Mark Me Enclosing_Mark N Number Nd Decimal_Number digit Nl Letter_Number No Other_Number
P Punctuation punct Pc Connector_Punctuation Pd Dash_Punctuation Ps Open_Punctuation Pe Close_Punctuation Pi Initial_Punctuation Pf Final_Punctuation Po Other_Punctuation
S Symbol Sm Math_Symbol Sc Currency_Symbol Sk Modifier_Symbol So Other_Symbol Z Separator Zs Space_Separator Zl Line_Separator Zp Paragraph_Separator
C Other Cc Control cntrl Cf Format Cs Surrogate Co Private_Use Cn Unassigned""".split()
BINARY = """ASCII ASCII_Hex_Digit AHex Alphabetic Alpha Any Assigned Bidi_Control Bidi_C Bidi_Mirrored Bidi_M Case_Ignorable CI Cased
Changes_When_Casefolded CWCF Changes_When_Casemapped CWCM Changes_When_Lowercased CWL Changes_When_NFKC_Casefolded CWKCF
Changes_When_Titlecased CWT Changes_When_Uppercased CWU Dash Default_Ignorable_Code_Point DI Deprecated Dep Diacritic Dia Emoji
Emoji_Component EComp Emoji_Modifier EMod Emoji_Modifier_Base EBase Emoji_Presentation EPres Extended_Pictographic ExtPict Extender Ext
Grapheme_Base Gr_Base Grapheme_Extend Gr_Ext Hex_Digit Hex IDS_Binary_Operator IDSB IDS_Trinary_Operator IDST ID_Continue IDC ID_Start IDS
Ideographic Ideo Join_Control Join_C Logical_Order_Exception LOE Lowercase Lower Math Noncharacter_Code_Point NChar Pattern_Syntax Pat_Syn
Pattern_White_Space Pat_WS Quotation_Mark QMark Radical Regional_Indicator RI Sentence_Terminal STerm Soft_Dotted SD Terminal_Punctuation Term
Unified_Ideograph UIdeo Uppercase Upper Variation_Selector VS White_Space space XID_Continue XIDC XID_Start XIDS""".split()
SCRIPTS = """Adlam Adlm Ahom Anatolian_Hieroglyphs Hluw Arabic Arab Armenian Armn Avestan Avst Balinese Bali Bamum Bamu Bassa_Vah Bass Batak Batk
Bengali Beng Beria_Erfe Berf Bhaiksuki Bhks Bopomofo Bopo Brahmi Brah Braille Brai Buginese Bugi Buhid Buhd Canadian_Aboriginal Cans Carian Cari
Caucasian_Albanian Aghb Chakma Cakm Cham Cherokee Cher Chisoi Chorasmian Chrs Common Zyyy Coptic Copt Qaac Cuneiform Xsux Cypriot Cprt Cypro_Minoan Cpmn
Cyrillic Cyrl Deseret Dsrt Devanagari Deva Dives_Akuru Diak Dogra Dogr Duployan Dupl Egyptian_Hieroglyphs Egyp Elbasan Elba Elymaic Elym Ethiopic Ethi
Garay Gara Georgian Geor Glagolitic Glag Gothic Goth Grantha Gran Greek Grek Gujarati Gujr Gunjala_Gondi Gong Gurmukhi Guru Gurung_Khema Gukh Han Hani Hangul Hang
Hanifi_Rohingya Rohg Hanunoo Hano Hatran Hatr Hebrew Hebr Hiragana Hira Imperial_Aramaic Armi Inherited Zinh Qaai Inscriptional_Pahlavi Phli
Inscriptional_Parthian Prti Javanese Java Kaithi Kthi Kannada Knda Katakana Kana Katakana_Or_Hiragana Hrkt Kawi Kayah_Li Kali Kharoshthi Khar Khitan_Small_Script Kits
Khmer Khmr Khojki Khoj Khudawadi Sind Kirat_Rai Krai Lao Laoo Latin Latn Lepcha Lepc Limbu Limb Linear_A Lina Linear_B Linb Lisu Lycian Lyci Lydian Lydi
Mahajani Mahj Makasar Maka Malayalam Mlym Mandaic Mand Manichaean Mani Marchen Marc Masaram_Gondi Gonm Medefaidrin Medf Meetei_Mayek Mtei Mende_Kikakui Mend
Meroitic_Cursive Merc Meroitic_Hieroglyphs Mero Miao Plrd Modi Mongolian Mong Mro Mroo Multani Mult Myanmar Mymr Nabataean Nbat Nag_Mundari Nagm Nandinagari Nand
New_Tai_Lue Talu Newa Nko Nkoo Nushu Nshu Nyiakeng_Puachue_Hmong Hmnp Ogham Ogam Ol_Chiki Olck Ol_Onal Onao Old_Hungarian Hung Old_Italic Ital Old_North_Arabian Narb
Old_Permic Perm Old_Persian Xpeo Old_Sogdian Sogo Old_South_Arabian Sarb Old_Turkic Orkh Old_Uyghur Ougr Oriya Orya Osage Osge Osmanya Osma Pahawh_Hmong Hmng
Palmyrene Palm Pau_Cin_Hau Pauc Phags_Pa Phag Phoenician Phnx Psalter_Pahlavi Phlp Rejang Rjng Runic Runr Samaritan Samr Saurashtra Saur Sharada Shrd Shavian Shaw
Sidetic Sidt Siddham Sidd SignWriting Sgnw Sinhala Sinh Sogdian Sogd Sora_Sompeng Sora Soyombo Soyo Sundanese Sund Sunuwar Sunu Syloti_Nagri Sylo Syriac Syrc Tagalog Tglg
Tagbanwa Tagb Tai_Le Tale Tai_Tham Lana Tai_Viet Tavt Tai_Yo Tayo Takri Takr Tamil Taml Tangsa Tnsa Tangut Tang Telugu Telu Thaana Thaa Thai Tibetan Tibt Tifinagh Tfng
Tirhuta Tirh Todhri Todr Tolong_Siki Tols Toto Tulu_Tigalari Tutg Ugaritic Ugar Vai Vaii Vithkuqi Vith Wancho Wcho Warang_Citi Wara Yezidi Yezi Yi Yiii Zanabazar_Square Zanb""".split()

DUMP = r"""
const fs = require('fs');
const spec = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const MAX = 0x10ffff;
const props = {};
function ranges(re) {
  const out = [];
  let start = -1;
  for (let c = 0; c <= MAX + 1; c++) {
    const hit = c <= MAX && re.test(String.fromCodePoint(c));
    if (hit && start < 0) start = c;
    if (!hit && start >= 0) { out.push(start, c - 1); start = -1; }
  }
  return out;
}
const cache = new Map();
function add(key, source) {
  let re;
  try { re = new RegExp(source, 'u'); } catch { return; }
  props[key] = ranges(re);
}
for (const n of spec.gc) add('gc:' + n, `\\p{General_Category=${n}}`);
for (const n of spec.binary) add('b:' + n, `\\p{${n}}`);
for (const n of spec.scripts) { add('sc:' + n, `\\p{Script=${n}}`); add('scx:' + n, `\\p{Script_Extensions=${n}}`); }

const seq = (s) => Array.from(s, (c) => c.codePointAt(0));
const upper = [], lower = [], norm = [], comps = [];
for (let c = 0x80; c <= MAX; c++) {
  if (c >= 0xd800 && c <= 0xdfff) continue;
  const s = String.fromCodePoint(c);
  const u = s.toUpperCase(), l = s.toLowerCase();
  if (u !== s) upper.push([c, seq(u)]);
  if (l !== s) lower.push([c, seq(l)]);
  if (c >= 0xac00 && c <= 0xd7a3) continue;
  const d = s.normalize('NFD'), k = s.normalize('NFKD');
  if (d !== s || k !== s) norm.push([c, d !== s ? seq(d) : null, k !== d ? seq(k) : null]);
  if (d !== s && s.normalize('NFC') === s) {
    const parts = seq(d);
    const last = parts.pop();
    const prefix = String.fromCodePoint(...parts).normalize('NFC');
    if (seq(prefix).length === 1) comps.push([seq(prefix)[0], last, c]);
  }
}

// the characters /i treats alike, by asking the regular expressions themselves
function classes(flags, limit) {
  const parent = new Map();
  const find = (x) => { while (parent.has(x) && parent.get(x) !== x) x = parent.get(x); return x; };
  const join = (a, b) => { a = find(a); b = find(b); if (a !== b) { if (a < b) parent.set(b, a); else parent.set(a, b); if (!parent.has(a)) parent.set(a, a); if (!parent.has(b)) parent.set(b, b); } };
  const test = (a, b) => {
    const esc = '\\u{' + a.toString(16) + '}';
    const src = flags.includes('u') ? `^${esc}$` : `^\\u${a.toString(16).padStart(4, '0')}$`;
    return new RegExp(src, flags).test(String.fromCodePoint(b));
  };
  for (let c = 0; c <= limit; c++) {
    if (c >= 0xd800 && c <= 0xdfff) continue;
    const s = String.fromCodePoint(c);
    const cand = new Set();
    for (const t of [s.toUpperCase(), s.toLowerCase(), s.toUpperCase().toLowerCase(), s.toLowerCase().toUpperCase()]) {
      const q = seq(t);
      if (q.length === 1 && q[0] !== c && q[0] <= limit) cand.add(q[0]);
    }
    for (const d of cand) if (test(c, d)) join(c, d);
  }
  const groups = new Map();
  for (const x of parent.keys()) {
    const r = find(x);
    if (!groups.has(r)) groups.set(r, []);
    groups.get(r).push(x);
  }
  return [...groups.values()].filter((g) => g.length > 1).map((g) => g.sort((a, b) => a - b));
}
const iClasses = classes('i', 0xffff), uClasses = classes('iu', MAX);
fs.writeFileSync(process.argv[3], JSON.stringify({ props, upper, lower, norm, comps, iClasses, uClasses, unicode: process.versions.unicode }));
"""

B36 = "0123456789abcdefghijklmnopqrstuvwxyz"


def b36(n):
    if n == 0:
        return "0"
    s = ""
    while n:
        s = B36[n % 36] + s
        n //= 36
    return s


def literal(text):
    # long literals in pieces the source can hold
    return "\n".join('        "' + text[i : i + 180] + '"' for i in range(0, len(text), 180)) or '        ""'


def encode_ranges(r):
    out, prev = [], 0
    for i in range(0, len(r), 2):
        out.append(b36(r[i] - prev) + "," + b36(r[i + 1] - r[i]))
        prev = r[i + 1]
    return " ".join(out)


def enc_seq(s):
    return ".".join(b36(c) for c in s)


with tempfile.TemporaryDirectory() as tmp:
    script = os.path.join(tmp, "dump.js")
    spec = os.path.join(tmp, "spec.json")
    result = os.path.join(tmp, "out.json")
    open(script, "w", encoding="utf-8").write(DUMP)
    json.dump({"gc": GC, "binary": BINARY, "scripts": SCRIPTS}, open(spec, "w"))
    subprocess.run([NODE, script, spec, result], check=True)
    data = json.load(open(result, encoding="utf-8"))

tables, index, entries = [], {}, []
for key, r in data["props"].items():
    text = encode_ranges(r)
    if text not in index:
        index[text] = len(tables)
        tables.append(text)
    entries.append((key, index[text]))

upper = {c: s for c, s in data["upper"]}
lower = {c: s for c, s in data["lower"]}
case_text = ";".join(b36(c) + ":" + enc_seq(upper.get(c, [])) + ":" + enc_seq(lower.get(c, [])) for c in sorted(set(upper) | set(lower)))
norm_text = ";".join(b36(c) + ":" + (enc_seq(d) if d else "") + ":" + (enc_seq(k) if k else "") for c, d, k in data["norm"])
comp_text = ";".join(".".join(b36(x) for x in t) for t in data["comps"])
ccc_text = ";".join(b36(c) + ":" + b36(unicodedata.combining(chr(c))) for c in range(0x110000) if unicodedata.combining(chr(c)))
i_text = ";".join(".".join(b36(x) for x in g) for g in data["iClasses"])
u_text = ";".join(".".join(b36(x) for x in g) for g in data["uClasses"])

out = ["// quantum: cpp", f"// Generated by src/tools/gen_js_unicode.py from Node.js (Unicode {data['unicode']}). Do not edit.", '#include "../js/JsUnicode.h.sa"', "", "namespace js", "{", ""]
out.append("const char *const kUniTables[] = {")
for t in tables:
    out.append(literal(t) + ",")
out.append("};")
out.append("")
out.append("const UniEntry kUniEntries[] = {")
for k, i in sorted(entries):
    out.append('    {"' + k + '", ' + str(i) + "},")
out.append("};")
out.append("const int kUniEntryCount = " + str(len(entries)) + ";")
for name, text in [("kUniCase", case_text), ("kUniNorm", norm_text), ("kUniComp", comp_text), ("kUniCcc", ccc_text), ("kUniIClasses", i_text), ("kUniUClasses", u_text)]:
    out.append("const char *const " + name + " =")
    out.append(literal(text) + ";")
out.append("")
out.append("} // namespace js")
open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print(OUT, sum(len(l) for l in out), "bytes;", len(tables), "tables,", len(entries), "names; Unicode", data["unicode"])
