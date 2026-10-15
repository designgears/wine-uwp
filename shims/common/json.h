/* A small JSON reader, shared by the shims that talk to web services (xboxauth, gameplatform). */
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Json
{
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json &operator[](const char *key) const
    {
        static const Json none;
        if (type == OBJ) for (auto &kv : o) if (kv.first == key) return kv.second;
        return none;
    }
    const Json &at(size_t i) const
    {
        static const Json none;
        return type == ARR && i < a.size() ? a[i] : none;
    }
    std::string str() const
    {
        if (type == STR) return s;
        if (type == NUM) { char b2[64]; snprintf(b2, sizeof(b2), "%.0f", n); return b2; }
        return std::string();
    }
};

struct JsonParser
{
    const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void put_utf8(std::string &o, unsigned cp)
    {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xc0 | cp >> 6); o += (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { o += (char)(0xe0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
        else { o += (char)(0xf0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 0x3f)); o += (char)(0x80 | (cp >> 6 & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
    }
    bool string(std::string &out)
    {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end)
            {
                p++;
                switch (*p)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    if (end - p < 5) return false;
                    unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xd800 && cp < 0xdc00 && end - p > 6 && p[1] == '\\' && p[2] == 'u')
                    {
                        unsigned lo = (unsigned)strtoul(std::string(p + 3, 4).c_str(), nullptr, 16);
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += *p; break;
                }
                p++;
            }
            else out += *p++;
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json &v, int depth)
    {
        if (depth > 64) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{')
        {
            v.type = Json::OBJ;
            p++; ws();
            if (p < end && *p == '}') { p++; return true; }
            for (;;)
            {
                std::string key;
                ws();
                if (!string(key)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                p++;
                v.o.emplace_back(key, Json());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[')
        {
            v.type = Json::ARR;
            p++; ws();
            if (p < end && *p == ']') { p++; return true; }
            for (;;)
            {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.type = Json::STR; return string(v.s); }
        if (!strncmp(p, "true", 4)) { v.type = Json::BOOL; v.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.type = Json::BOOL; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char *e;
        v.n = strtod(p, &e);
        if (e == p) return false;
        v.type = Json::NUM;
        p = e;
        return true;
    }
};

static Json parse_json(const std::string &text)
{
    Json v;
    JsonParser jp{ text.data(), text.data() + text.size() };
    if (!jp.value(v, 0)) return Json();
    return v;
}
