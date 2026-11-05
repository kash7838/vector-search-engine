#ifndef VECTOR_ENGINE_HPP
#define VECTOR_ENGINE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
struct SearchResult {
    int id;
    float score;
    // Min-heap ordering: lowest score sits at the top
    bool operator>(const SearchResult& other) const { return score > other.score; }
};

// In-memory vectors (row-major) produced by the CSV / JSON loaders
struct VectorData {
    int32_t dim = 0;
    std::vector<float> data;
    size_t count() const { return dim > 0 ? data.size() / static_cast<size_t>(dim) : 0; }
};

// ---------------------------------------------------------------------------
// Binary database format: [int32 count][int32 dim][count*dim floats]
// ---------------------------------------------------------------------------
inline bool saveDatabase(const std::string& filename, const VectorData& vd, std::string& err) {
    if (vd.dim <= 0 || vd.count() == 0) { err = "No vectors to save."; return false; }
    if (vd.count() > static_cast<size_t>(INT32_MAX)) { err = "Too many vectors."; return false; }
    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out) { err = "Cannot write " + filename; return false; }
    int32_t n = static_cast<int32_t>(vd.count());
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    out.write(reinterpret_cast<const char*>(&vd.dim), sizeof(vd.dim));
    out.write(reinterpret_cast<const char*>(vd.data.data()),
              static_cast<std::streamsize>(vd.data.size() * sizeof(float)));
    out.close();
    if (!out) { err = "Write failed (disk full?)"; return false; }
    return true;
}

inline bool generateMockDatabase(const std::string& filename, int num_vectors, int dimension) {
    VectorData vd;
    vd.dim = dimension;
    vd.data.resize(static_cast<size_t>(num_vectors) * dimension);
    std::mt19937 gen(42);
    std::uniform_real_distribution<float> dist(-10.0f, 10.0f);
    for (auto& v : vd.data) v = dist(gen);
    std::string err;
    return saveDatabase(filename, vd, err);
}

// ---------------------------------------------------------------------------
// RAII wrapper around the mmap'd database (validates header vs. file size)
// ---------------------------------------------------------------------------
class MappedDatabase {
public:
    ~MappedDatabase() { close(); }

    bool open(const std::string& filename, std::string& err) {
        close();
        fd_ = ::open(filename.c_str(), O_RDONLY);
        if (fd_ == -1) { err = "Cannot open " + filename; return false; }
        struct stat sb;
        if (fstat(fd_, &sb) == -1 || sb.st_size < 8) { err = "Database file is empty or corrupt."; close(); return false; }
        size_ = static_cast<size_t>(sb.st_size);
        map_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (map_ == MAP_FAILED) { map_ = nullptr; err = "mmap failed."; close(); return false; }
        int32_t h[2];
        std::memcpy(h, map_, sizeof(h));
        uint64_t expected = 8ull + static_cast<uint64_t>(std::max(h[0], 0)) * std::max(h[1], 0) * sizeof(float);
        if (h[0] <= 0 || h[1] <= 0 || expected != size_) {
            err = "Header does not match file size (corrupt database).";
            close();
            return false;
        }
        count_ = h[0];
        dim_ = h[1];
        return true;
    }

    void close() {
        if (map_) { munmap(map_, size_); map_ = nullptr; }
        if (fd_ != -1) { ::close(fd_); fd_ = -1; }
        size_ = 0; count_ = 0; dim_ = 0;
    }

    bool isOpen() const { return map_ != nullptr; }
    int32_t count() const { return count_; }
    int32_t dim() const { return dim_; }
    size_t sizeBytes() const { return size_; }
    const float* vectors() const { return reinterpret_cast<const float*>(static_cast<const char*>(map_) + 8); }

private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t size_ = 0;
    int32_t count_ = 0, dim_ = 0;
};

// ---------------------------------------------------------------------------
// Similarity + Top-K search
// ---------------------------------------------------------------------------
inline float computeCosineSimilarity(const float* a, const float* b, int dim) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0.0f || nb == 0.0f) return 0.0f;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

// Query norm is computed once instead of once per database vector.
inline std::vector<SearchResult> searchTopK(const float* vectors_ptr, size_t num_vectors, int dimension,
                                            const std::vector<float>& query, int k) {
    std::vector<SearchResult> results;
    if (k <= 0 || num_vectors == 0 || query.size() != static_cast<size_t>(dimension)) return results;

    float qn = 0.0f;
    for (float x : query) qn += x * x;
    qn = std::sqrt(qn);

    std::priority_queue<SearchResult, std::vector<SearchResult>, std::greater<SearchResult>> heap;
    for (size_t i = 0; i < num_vectors; ++i) {
        const float* v = vectors_ptr + i * static_cast<size_t>(dimension);
        float dot = 0.0f, nb = 0.0f;
        for (int d = 0; d < dimension; ++d) {
            dot += query[d] * v[d];
            nb += v[d] * v[d];
        }
        float score = (qn == 0.0f || nb == 0.0f) ? 0.0f : dot / (qn * std::sqrt(nb));
        if (!std::isfinite(score)) score = 0.0f;

        if (heap.size() < static_cast<size_t>(k)) {
            heap.push({static_cast<int>(i), score});
        } else if (score > heap.top().score) {
            heap.pop();
            heap.push({static_cast<int>(i), score});
        }
    }
    while (!heap.empty()) { results.push_back(heap.top()); heap.pop(); }
    std::reverse(results.begin(), results.end());  // highest score first
    return results;
}

// ---------------------------------------------------------------------------
// CSV loader: one vector per row; comma / semicolon / tab separated.
// A non-numeric first row is treated as a header and skipped.
// ---------------------------------------------------------------------------
inline bool parseCSV(const std::string& text, VectorData& out, std::string& err) {
    out.dim = 0;
    out.data.clear();
    size_t pos = (text.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;  // skip UTF-8 BOM
    size_t line_no = 0;
    bool seen_row = false;
    std::vector<float> row;

    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t,;") == std::string::npos) continue;  // blank

        row.clear();
        bool numeric = true;
        const char* p = line.c_str();
        while (true) {
            while (*p == ' ') ++p;
            char* end = nullptr;
            double v = std::strtod(p, &end);
            if (end == p || !std::isfinite(v)) { numeric = false; break; }
            row.push_back(static_cast<float>(v));
            p = end;
            while (*p == ' ') ++p;
            if (*p == ',' || *p == ';' || *p == '\t') { ++p; if (*p == '\0') break; continue; }
            if (*p == '\0') break;
            numeric = false;
            break;
        }

        if (!numeric) {
            if (!seen_row) { seen_row = true; continue; }  // header row
            err = "Line " + std::to_string(line_no) + ": non-numeric value.";
            return false;
        }
        seen_row = true;
        if (out.dim == 0) out.dim = static_cast<int32_t>(row.size());
        else if (row.size() != static_cast<size_t>(out.dim)) {
            err = "Line " + std::to_string(line_no) + ": expected " + std::to_string(out.dim) +
                  " values but found " + std::to_string(row.size()) + ".";
            return false;
        }
        out.data.insert(out.data.end(), row.begin(), row.end());
    }
    if (out.count() == 0) { err = "No numeric rows found in CSV."; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser (numeric arrays are stored compactly in `nums`)
// ---------------------------------------------------------------------------
struct JsonValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    std::string str;
    std::vector<float> nums;                          // Arr made only of numbers
    std::vector<JsonValue> arr;                       // Arr of other values
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool isNumArray() const { return type == Arr && arr.empty(); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(JsonValue& out, std::string& err) {
        try {
            i_ = (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
            value(out, 0);
            ws();
            if (i_ != s_.size()) fail("unexpected trailing data");
            return true;
        } catch (const std::runtime_error& e) {
            err = std::string("Invalid JSON: ") + e.what();
            return false;
        }
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m + " at offset " + std::to_string(i_)); }
    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    char peek() { ws(); if (i_ >= s_.size()) fail("unexpected end"); return s_[i_]; }
    bool isNumStart(char c) { return c == '-' || (c >= '0' && c <= '9'); }

    double number() {
        const char* start = s_.c_str() + i_;
        char* end = nullptr;
        double v = std::strtod(start, &end);
        if (end == start || !std::isfinite(v)) fail("bad number");
        i_ += static_cast<size_t>(end - start);
        return v;
    }

    std::string string_() {
        ++i_;  // opening quote
        std::string r;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                ++i_;
                char c = s_[i_];
                r += (c == 'n') ? '\n' : (c == 't') ? '\t' : c;  // \uXXXX kept loosely; keys are ASCII
            } else r += s_[i_];
            ++i_;
        }
        if (i_ >= s_.size()) fail("unterminated string");
        ++i_;
        return r;
    }

    void value(JsonValue& v, int depth) {
        if (depth > 64) fail("nesting too deep");
        char c = peek();
        if (c == '[') {
            ++i_;
            v.type = JsonValue::Arr;
            if (peek() == ']') { ++i_; return; }
            while (true) {
                if (isNumStart(peek())) {
                    if (!v.arr.empty()) fail("mixed array");
                    v.nums.push_back(static_cast<float>(number()));
                } else {
                    if (!v.nums.empty()) fail("mixed array");
                    v.arr.emplace_back();
                    value(v.arr.back(), depth + 1);
                }
                char d = peek();
                ++i_;
                if (d == ']') break;
                if (d != ',') fail("expected ',' or ']'");
            }
        } else if (c == '{') {
            ++i_;
            v.type = JsonValue::Obj;
            if (peek() == '}') { ++i_; return; }
            while (true) {
                if (peek() != '"') fail("expected key");
                std::string key = string_();
                if (peek() != ':') fail("expected ':'");
                ++i_;
                v.obj.emplace_back(key, JsonValue());
                value(v.obj.back().second, depth + 1);
                char d = peek();
                ++i_;
                if (d == '}') break;
                if (d != ',') fail("expected ',' or '}'");
            }
        } else if (c == '"') {
            v.type = JsonValue::Str;
            v.str = string_();
        } else if (isNumStart(c)) {
            v.type = JsonValue::Num;
            v.num = number();
        } else if (s_.compare(i_, 4, "true") == 0)  { v.type = JsonValue::Bool; i_ += 4; }
        else if (s_.compare(i_, 5, "false") == 0)   { v.type = JsonValue::Bool; i_ += 5; }
        else if (s_.compare(i_, 4, "null") == 0)    { v.type = JsonValue::Null; i_ += 4; }
        else fail("unexpected character");
    }
};

// Accepted JSON shapes:
//   [[1,2,3],[4,5,6]]
//   [{"vector":[1,2,3]}, {"embedding":[4,5,6]}, {"values":[...]}]
//   {"vectors":[ ...either of the above... ]}   (also "embeddings" / "data")
//   [1,2,3]   (a single vector)
inline bool parseJSON(const std::string& text, VectorData& out, std::string& err) {
    out.dim = 0;
    out.data.clear();
    JsonValue root;
    JsonParser parser(text);
    if (!parser.parse(root, err)) return false;

    const JsonValue* list = &root;
    if (root.type == JsonValue::Obj) {
        list = nullptr;
        for (const char* key : {"vectors", "embeddings", "data"})
            if ((list = root.get(key))) break;
        if (!list) { err = "JSON object needs a \"vectors\", \"embeddings\" or \"data\" array."; return false; }
    }
    if (list->type != JsonValue::Arr) { err = "Expected a JSON array of vectors."; return false; }

    auto addRow = [&](const std::vector<float>& row, size_t index) -> bool {
        if (row.empty()) { err = "Vector #" + std::to_string(index) + " is empty."; return false; }
        if (out.dim == 0) out.dim = static_cast<int32_t>(row.size());
        else if (row.size() != static_cast<size_t>(out.dim)) {
            err = "Vector #" + std::to_string(index) + " has " + std::to_string(row.size()) +
                  " values, expected " + std::to_string(out.dim) + ".";
            return false;
        }
        out.data.insert(out.data.end(), row.begin(), row.end());
        return true;
    };

    if (!list->nums.empty()) {  // single flat vector
        if (!addRow(list->nums, 0)) return false;
    } else {
        for (size_t i = 0; i < list->arr.size(); ++i) {
            const JsonValue& e = list->arr[i];
            if (e.type == JsonValue::Arr && e.arr.empty()) { if (!addRow(e.nums, i)) return false; }
            else if (e.type == JsonValue::Obj) {
                const JsonValue* v = nullptr;
                for (const char* key : {"vector", "embedding", "values"})
                    if ((v = e.get(key))) break;
                if (!v || v->type != JsonValue::Arr || !v->arr.empty()) {
                    err = "Item #" + std::to_string(i) + " needs a numeric \"vector\", \"embedding\" or \"values\" array.";
                    return false;
                }
                if (!addRow(v->nums, i)) return false;
            } else {
                err = "Item #" + std::to_string(i) + " is not a numeric array or object.";
                return false;
            }
        }
    }
    if (out.count() == 0) { err = "No vectors found in JSON."; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Dispatch by extension, falling back to sniffing the first character
// ---------------------------------------------------------------------------
inline bool loadVectorText(const std::string& filename, const std::string& text, VectorData& out, std::string& err) {
    std::string ext;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    }
    if (ext == "json") return parseJSON(text, out, err);
    if (ext == "csv" || ext == "txt" || ext == "tsv") return parseCSV(text, out, err);
    size_t f = text.find_first_not_of(" \t\r\n");
    if (f != std::string::npos && (text[f] == '[' || text[f] == '{')) return parseJSON(text, out, err);
    return parseCSV(text, out, err);
}

inline bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

#endif // VECTOR_ENGINE_HPP