#include "QpmManifest.h"
#include "QpmSemver.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        const char *const kManifestNames[] = {"package.qon", "package.json", "requirements.txt"};

        std::string trim(const std::string &s)
        {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
                ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
                --e;
            return s.substr(b, e - b);
        }

        bool startsWith(const std::string &s, const std::string &prefix)
        {
            return s.compare(0, prefix.size(), prefix) == 0;
        }

        void stripBom(std::string &text)
        {
            if (startsWith(text, "\xEF\xBB\xBF"))
                text.erase(0, 3);
        }

        // Splits on '\n', dropping a trailing '\r' from each line.
        std::vector<std::string> splitLines(const std::string &text)
        {
            std::vector<std::string> lines;
            std::string line;
            std::istringstream in(text);
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
            return lines;
        }

        bool isNameChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/';
        }

        bool isNumeric(const std::string &s)
        {
            if (s.empty())
                return false;
            for (char c : s)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    return false;
            return true;
        }

        std::vector<std::string> splitOn(const std::string &s, char sep)
        {
            std::vector<std::string> parts;
            std::string part;
            std::istringstream in(s);
            while (std::getline(in, part, sep))
                parts.push_back(part);
            return parts;
        }

        // "1.4" -> "1.4.0" when every part is a number; anything else unchanged.
        std::string padVersion(const std::string &v)
        {
            std::vector<std::string> parts = splitOn(v, '.');
            for (const auto &p : parts)
                if (!isNumeric(p))
                    return v;
            std::string out = v;
            for (size_t n = parts.size(); n < 3; ++n)
                out += ".0";
            return out;
        }

        // pip's compatible-release clause: ~=1.4.2 -> >=1.4.2 <1.5.0, ~=1.4 -> >=1.4.0 <2.0.0
        std::string compatibleRelease(const std::string &v)
        {
            std::vector<std::string> parts = splitOn(v, '.');
            if (parts.size() < 2 || !isNumeric(parts[parts.size() - 2]))
                return ">=" + padVersion(v);
            parts.pop_back();
            parts.back() = std::to_string(std::stoi(parts.back()) + 1);
            std::string upper;
            for (size_t i = 0; i < parts.size(); ++i)
                upper += (i ? "." : "") + parts[i];
            return ">=" + padVersion(v) + " <" + padVersion(upper);
        }

        std::string sectionKey(const std::string &raw)
        {
            std::string lower;
            for (char c : raw)
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "dependencies" || lower == "deps")
                return "dependencies";
            if (lower == "devdependencies" || lower == "dev-dependencies" || lower == "dev_dependencies" ||
                lower == "dev")
                return "devDependencies";
            if (lower == "package")
                return "";
            return raw;
        }

        bool isDependencySection(const std::string &section)
        {
            return section == "dependencies" || section == "devDependencies";
        }

        // pip treats '#' as a comment at line start or after whitespace.
        std::string stripComment(const std::string &line)
        {
            for (size_t i = 0; i < line.size(); ++i)
                if (line[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))))
                    return line.substr(0, i);
            return line;
        }

        // One requirement line -> (name, npm range). False if it isn't one.
        bool parseRequirement(const std::string &line, std::string &name, std::string &range)
        {
            std::string s = line;
            size_t marker = s.find(';'); // environment markers don't apply to npm packages
            if (marker != std::string::npos)
                s = trim(s.substr(0, marker));

            size_t i = 0;
            if (i < s.size() && s[i] == '@')
                ++i; // npm scope
            while (i < s.size() && isNameChar(s[i]))
                ++i;
            name = s.substr(0, i);
            if (name.empty() || name == "@")
                return false;

            if (i < s.size() && s[i] == '[') // pip extras
            {
                size_t close = s.find(']', i);
                i = close == std::string::npos ? s.size() : close + 1;
            }
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            if (i < s.size() && (s[i] == '@' || s[i] == ':'))
                ++i;
            range = toNpmRange(trim(s.substr(i)));
            return true;
        }

        JsonValue parseValue(const std::string &raw)
        {
            if (!raw.empty() && (raw[0] == '[' || raw[0] == '{' || raw[0] == '"'))
                return JsonValue::parse(raw);
            return JsonValue(raw);
        }

        struct LineInfo
        {
            std::string section; // "dependencies", "devDependencies", "" for [package], or a raw name
            bool isHeader = false;
            std::string depName; // set on a dependency line
        };

        // Parses requirements-style text into `data`, describing every line in `info`.
        void parseRequirementsText(const std::string &text, JsonValue &data, std::vector<LineInfo> &info)
        {
            data = JsonValue::makeObject();
            std::vector<std::string> lines = splitLines(text);
            std::string section = "dependencies";
            for (size_t n = 0; n < lines.size(); ++n)
            {
                LineInfo li;
                std::string line = trim(stripComment(lines[n]));
                auto fail = [&](const std::string &why) {
                    throw std::runtime_error("line " + std::to_string(n + 1) + ": " + why);
                };

                if (!line.empty() && line[0] == '[' && line.back() == ']')
                {
                    section = sectionKey(trim(line.substr(1, line.size() - 2)));
                    li.isHeader = true;
                }
                else if (line.empty() || (isDependencySection(section) && line[0] == '-'))
                {
                    // blank, or a pip option (-r, --index-url, ...) that has no npm meaning
                }
                else if (isDependencySection(section))
                {
                    std::string name, range;
                    if (!parseRequirement(line, name, range))
                        fail("expected a package name, got \"" + line + "\"");
                    JsonValue deps = data.get(section, JsonValue::makeObject());
                    deps.set(name, range);
                    data.set(section, deps);
                    li.depName = name;
                }
                else
                {
                    size_t eq = line.find('=');
                    if (eq == std::string::npos)
                        fail("expected `key = value` in [" + (section.empty() ? "package" : section) + "]");
                    std::string key = trim(line.substr(0, eq));
                    JsonValue value;
                    try
                    {
                        value = parseValue(trim(line.substr(eq + 1)));
                    }
                    catch (const std::exception &e)
                    {
                        fail("bad value for \"" + key + "\": " + e.what());
                    }
                    if (section.empty())
                    {
                        data.set(key, value);
                    }
                    else
                    {
                        JsonValue obj = data.get(section, JsonValue::makeObject());
                        obj.set(key, value);
                        data.set(section, obj);
                    }
                }
                li.section = section;
                info.push_back(li);
            }
        }

        bool looksLikeJson(const std::string &text)
        {
            for (char c : text)
                if (!std::isspace(static_cast<unsigned char>(c)))
                    return c == '{';
            return false;
        }

        // How a dependency is written back as a line. requirements.txt gets
        // pip syntax where it can express the range, so pip-aware tools still read it.
        std::string requirementLine(const std::string &name, const std::string &range, bool pipStyle)
        {
            if (range.empty() || range == "latest" || range == "*")
                return name;
            if (!pipStyle)
                return name + " " + range;
            SemVer v;
            if (SemVer::tryParse(range, v))
                return name + "==" + range;
            if (range[0] == '^' && SemVer::tryParse(range.substr(1), v))
            {
                std::string upper = v.major > 0   ? std::to_string(v.major + 1) + ".0.0"
                                    : v.minor > 0 ? "0." + std::to_string(v.minor + 1) + ".0"
                                                  : "0.0." + std::to_string(v.patch + 1);
                return name + ">=" + range.substr(1) + ",<" + upper;
            }
            return name + " " + range;
        }
    } // namespace

    std::string toNpmRange(const std::string &spec)
    {
        std::string s = trim(spec);
        if (s.empty())
            return "latest";
        bool pip = s.find(',') != std::string::npos || startsWith(s, "==") || startsWith(s, "~=") ||
                   startsWith(s, "!=");
        if (!pip)
            return s;

        std::string out;
        for (std::string clause : splitOn(s, ','))
        {
            clause = trim(clause);
            std::string converted;
            if (startsWith(clause, "==="))
                converted = trim(clause.substr(3));
            else if (startsWith(clause, "=="))
            {
                converted = trim(clause.substr(2));
                if (converted.size() >= 2 && converted.compare(converted.size() - 2, 2, ".*") == 0)
                    converted.replace(converted.size() - 1, 1, "x");
            }
            else if (startsWith(clause, "~="))
                converted = compatibleRelease(trim(clause.substr(2)));
            else if (startsWith(clause, "!="))
                continue; // npm ranges can't exclude a single version
            else
                converted = clause;
            if (!converted.empty())
                out += (out.empty() ? "" : " ") + converted;
        }
        return out.empty() ? "*" : out;
    }

    fs::path findManifest(const fs::path &dir)
    {
        std::error_code ec;
        for (const char *name : kManifestNames)
            if (fs::is_regular_file(dir / name, ec))
                return dir / name;
        return {};
    }

    Manifest parseManifest(const std::string &rawText, const fs::path &fileName)
    {
        Manifest m;
        m.path = fileName;
        m.text = rawText;
        stripBom(m.text);

        std::string file = fileName.filename().string();
        bool json = file == "package.json" || (file != "requirements.txt" && looksLikeJson(m.text));
        try
        {
            if (json)
            {
                m.format = ManifestFormat::Json;
                m.data = JsonValue::parse(m.text);
                if (!m.data.isObject())
                    throw std::runtime_error("expected a JSON object");
            }
            else
            {
                m.format = ManifestFormat::Requirements;
                std::vector<LineInfo> info;
                parseRequirementsText(m.text, m.data, info);
            }
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(file + ": " + e.what());
        }
        return m;
    }

    Manifest loadManifest(const fs::path &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            throw std::runtime_error("can't read " + path.string());
        std::ostringstream ss;
        ss << f.rdbuf();
        return parseManifest(ss.str(), path);
    }

    void setDependency(Manifest &m, const std::string &name, const std::string &range)
    {
        JsonValue deps = m.data.get("dependencies", JsonValue::makeObject());
        deps.set(name, range);
        m.data.set("dependencies", deps);
        if (m.format != ManifestFormat::Requirements)
            return;

        std::string newLine = requirementLine(name, range, m.path.filename() == "requirements.txt");
        std::string eol = m.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines = splitLines(m.text);
        JsonValue ignored;
        std::vector<LineInfo> info;
        parseRequirementsText(m.text, ignored, info);

        // Replace an existing line for this package, else add one after the
        // last dependency (or right under an explicit [dependencies] header).
        int replaceAt = -1, insertAfter = -1;
        bool anyHeader = false;
        for (size_t n = 0; n < info.size(); ++n)
        {
            anyHeader = anyHeader || info[n].isHeader;
            if (info[n].section != "dependencies")
                continue;
            if (info[n].depName == name)
                replaceAt = static_cast<int>(n);
            if (!info[n].depName.empty() || info[n].isHeader)
                insertAfter = static_cast<int>(n);
        }

        if (replaceAt >= 0)
            lines[replaceAt] = newLine;
        else if (insertAfter >= 0)
            lines.insert(lines.begin() + insertAfter + 1, newLine);
        else if (!anyHeader)
            lines.push_back(newLine);
        else
        {
            if (!lines.empty() && !trim(lines.back()).empty())
                lines.push_back("");
            lines.push_back("[dependencies]");
            lines.push_back(newLine);
        }

        m.text.clear();
        for (const auto &line : lines)
            m.text += line + eol;
    }

    std::string serializeManifest(const Manifest &m)
    {
        if (m.format == ManifestFormat::Json)
            return m.data.stringify(2) + "\n";
        return m.text;
    }

} // namespace qpm
