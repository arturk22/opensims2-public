#include "ui_script.hpp"

#include "resindex.hpp"

#include <cctype>
#include <charconv>
#include <cstring>

namespace os2 {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && space(s.back()))
        s.remove_suffix(1);
    return s;
}

std::optional<uint32_t> hex(std::string_view s) {
    s = trim(s);
    if (s.starts_with("0x") || s.starts_with("0X"))
        s.remove_prefix(2);
    uint32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc() || p != s.data() + s.size() || s.empty())
        return std::nullopt;
    return v;
}

struct Parser {
    std::string_view text;
    size_t pos = 0;
    UiScript out;
    int line = 1;

    bool done() const { return pos >= text.size(); }
    char peek() const { return text[pos]; }
    char next() {
        char c = text[pos++];
        if (c == '\n')
            ++line;
        return c;
    }
    void warn(const std::string& msg) { out.warnings.push_back("line " + std::to_string(line) + ": " + msg); }

    // Reads one attribute value; the cursor is just past '='.
    std::string value() {
        std::string v;
        if (done())
            return v;
        if (peek() == '"') {
            next();
            while (!done() && peek() != '"')
                v += next();
            if (done())
                warn("unterminated string");
            else
                next();
            return v;
        }
        if (peek() == '(' || peek() == '{') {
            const char close = peek() == '(' ? ')' : '}';
            while (!done() && peek() != close && peek() != '>' && peek() != '\n')
                v += next();
            if (!done() && peek() == close)
                v += next();
            else
                warn("unterminated list " + v);
            // A few values are two lists back to back ("(127,136,255)(255,242,255)");
            // they stay one value and uiList reads the first.
            if (!done() && (peek() == '(' || peek() == '{'))
                v += value();
            return v;
        }
        while (!done() && !space(peek()) && peek() != '>')
            v += next();
        return v;
    }

    // Reads a tag; the cursor is just past '<'.
    void tag(std::vector<UiElement*>& stack, UiElement*& last) {
        std::string name;
        while (!done() && !space(peek()) && peek() != '>')
            name += next();
        UiElement el;
        el.tag = name;
        for (;;) {
            while (!done() && space(peek()))
                next();
            if (done()) {
                warn("unterminated tag <" + name);
                return;
            }
            if (peek() == '>') {
                next();
                break;
            }
            std::string key;
            while (!done() && peek() != '=' && !space(peek()) && peek() != '>')
                key += next();
            if (done() || peek() != '=') {
                // Bare words are flags ("transparent"); keep them with an empty value.
                if (key.empty() || !std::isalpha(static_cast<unsigned char>(key[0])))
                    warn("malformed attribute: " + key);
                else
                    el.attrs.emplace_back(std::move(key), std::string());
                continue;
            }
            next();
            el.attrs.emplace_back(std::move(key), value());
        }
        if (name == "CHILDREN") {
            if (!last)
                warn("CHILDREN without a window");
            else
                stack.push_back(last);
            last = nullptr;
            return;
        }
        if (name == "/CHILDREN") {
            if (stack.empty())
                warn("unbalanced /CHILDREN");
            else {
                last = stack.back();
                stack.pop_back();
            }
            return;
        }
        if (name != "LEGACY")
            warn("unknown tag <" + name + ">");
        auto& list = stack.empty() ? out.roots : stack.back()->children;
        list.push_back(std::move(el));
        last = &list.back();
    }
};

const UiElement* findIn(const std::vector<UiElement>& list, uint32_t id) {
    for (const auto& e : list) {
        if (e.attr("id") && e.id() == id)
            return &e;
        if (const auto* f = findIn(e.children, id))
            return f;
    }
    return nullptr;
}

size_t countIn(const std::vector<UiElement>& list) {
    size_t n = list.size();
    for (const auto& e : list)
        n += countIn(e.children);
    return n;
}

} // namespace

const std::string* UiElement::attr(std::string_view key) const {
    for (const auto& [k, v] : attrs)
        if (k == key)
            return &v;
    return nullptr;
}

std::string UiElement::get(std::string_view key, std::string_view fallback) const {
    const auto* v = attr(key);
    return v ? *v : std::string(fallback);
}

bool UiElement::flag(std::string_view key) const {
    const auto* v = attr(key);
    return v && *v == "yes";
}

uint32_t UiElement::id() const {
    const auto* v = attr("id");
    return v ? hex(*v).value_or(0) : 0;
}

std::array<int, 4> UiElement::area() const {
    const auto v = uiInts(get("area"));
    if (v.size() != 4)
        return {0, 0, 0, 0};
    return {v[0], v[1], v[2], v[3]};
}

UiScript UiScript::parse(std::string_view text) {
    Parser p;
    p.text = text;
    // Open CHILDREN blocks: only the innermost parent's children grow, so
    // pointers to the outer ones stay valid.
    std::vector<UiElement*> stack;
    UiElement* last = nullptr;
    bool lineStart = true;
    while (!p.done()) {
        char c = p.peek();
        if (c == '#' && lineStart) {
            while (!p.done() && p.peek() != '\n')
                p.next();
            continue;
        }
        if (c == '<') {
            p.next();
            p.tag(stack, last);
            lineStart = false;
            continue;
        }
        if (c == '\n')
            lineStart = true;
        else if (!space(c)) {
            p.warn(std::string("stray character '") + c + "'");
            lineStart = false;
        }
        p.next();
    }
    if (!stack.empty())
        p.warn(std::to_string(stack.size()) + " unclosed CHILDREN");
    return std::move(p.out);
}

size_t UiScript::elementCount() const { return countIn(roots); }

const UiElement* UiScript::find(uint32_t id) const { return findIn(roots, id); }

bool isReia(std::span<const uint8_t> data) {
    return data.size() >= 16 && std::memcmp(data.data(), "RIFF", 4) == 0 && std::memcmp(data.data() + 8, "Reia", 4) == 0;
}

std::vector<std::string> uiList(std::string_view value) {
    std::vector<std::string> out;
    value = trim(value);
    if (value.size() < 2 || !((value.front() == '(' && value.back() == ')') || (value.front() == '{' && value.back() == '}')))
        return out; // also rejects "(a)(b" and the like
    const char close = value.front() == '(' ? ')' : '}';
    value = value.substr(1, value.find(close) - 1);
    for (size_t start = 0; start <= value.size();) {
        size_t end = value.find(',', start);
        if (end == std::string_view::npos)
            end = value.size();
        out.emplace_back(trim(value.substr(start, end - start)));
        start = end + 1;
    }
    return out;
}

std::vector<int> uiInts(std::string_view value) {
    std::vector<int> out;
    for (const auto& item : uiList(value)) {
        int v = 0;
        const char* b = item.data();
        const char* e = b + item.size();
        int base = 10;
        if (item.starts_with("0x")) {
            b += 2;
            base = 16;
        }
        auto [p, ec] = std::from_chars(b, e, v, base);
        if (ec != std::errc() || p != e || b == e)
            return {};
        out.push_back(v);
    }
    return out;
}

std::optional<std::array<uint8_t, 3>> uiColor(std::string_view value) {
    const auto v = uiInts(value);
    if (v.size() != 3)
        return std::nullopt;
    std::array<uint8_t, 3> c{};
    for (int i = 0; i < 3; ++i) {
        if (v[size_t(i)] < 0 || v[size_t(i)] > 255)
            return std::nullopt;
        c[size_t(i)] = uint8_t(v[size_t(i)]);
    }
    return c;
}

std::optional<UiResourceRef> uiResourceRef(std::string_view value, uint32_t defaultType) {
    const auto items = uiList(value);
    std::vector<uint32_t> v;
    for (const auto& s : items) {
        auto h = hex(s);
        if (!h)
            return std::nullopt;
        v.push_back(*h);
    }
    if (v.size() == 2)
        return UiResourceRef{defaultType, v[0], v[1]};
    if (v.size() == 3)
        return UiResourceRef{v[0], v[1], v[2]};
    return std::nullopt;
}

std::optional<UiStringRef> uiStringRef(std::string_view value) {
    const auto ref = uiResourceRef(value, 0);
    if (!ref || ref->type != 0 || (ref->instance & 0xFFFF) == 0)
        return std::nullopt;
    return UiStringRef{ref->group, ref->instance >> 16, (ref->instance & 0xFFFF) - 1};
}

uint32_t packageGroupHash(std::string_view stem) { return (nameInstanceHash(stem) & 0xFFFFFF) | 0x7F000000; }

namespace {
const UiElement* findCaption(const std::vector<UiElement>& list, std::string_view caption) {
    for (const auto& e : list) {
        if (e.get("caption") == caption)
            return &e;
        if (const UiElement* c = findCaption(e.children, caption))
            return c;
    }
    return nullptr;
}
} // namespace

std::map<std::string, std::string> uiConstants(const UiScript& script, std::string_view table) {
    std::map<std::string, std::string> out;
    const UiElement* t = findCaption(script.roots, table);
    if (!t)
        return out;
    for (const auto& c : t->children) {
        const std::string text = c.get("caption");
        const size_t eq = text.find('=');
        if (eq != std::string::npos && eq > 0)
            out[text.substr(0, eq)] = text.substr(eq + 1);
    }
    return out;
}

} // namespace os2
