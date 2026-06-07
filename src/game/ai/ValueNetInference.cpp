#include "ValueNetInference.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>

namespace mtg {

namespace {

static bool readLE(std::ifstream& f, void* dst, std::size_t n) {
    return !!f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
}

static bool readI32(std::ifstream& f, int32_t& v) { return readLE(f, &v, 4); }
static bool readU32(std::ifstream& f, uint32_t& v) { return readLE(f, &v, 4); }

static bool readFloats(std::ifstream& f, std::vector<float>& v, int n) {
    v.resize(static_cast<size_t>(n));
    return readLE(f, v.data(), static_cast<std::size_t>(n) * sizeof(float));
}

} // namespace

bool ValueNetInference::load(const std::string& path) {
    m_loaded = false;
    m_layers.clear();
    m_lnWeight.clear();
    m_lnBias.clear();
    m_hasLN = false;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[ValueNet] Could not open: " << path << "\n";
        return false;
    }

    // Magic
    char magic[4];
    if (!readLE(f, magic, 4) || std::memcmp(magic, "CVNW", 4) != 0) {
        std::cerr << "[ValueNet] Bad magic in: " << path << "\n";
        return false;
    }

    int32_t version, stateSize, numLayers;
    if (!readI32(f, version) || !readI32(f, stateSize) || !readI32(f, numLayers)) {
        std::cerr << "[ValueNet] Truncated header in: " << path << "\n";
        return false;
    }
    if (version != 1) {
        std::cerr << "[ValueNet] Unsupported version " << version << " in: " << path << "\n";
        return false;
    }
    if (stateSize != kStateSize) {
        std::cerr << "[ValueNet] State size mismatch: file=" << stateSize
                  << " expected=" << kStateSize << "\n";
        return false;
    }

    m_layers.resize(static_cast<size_t>(numLayers));
    for (auto& layer : m_layers) {
        int32_t rows, cols;
        if (!readI32(f, rows) || !readI32(f, cols)) return false;
        layer.rows = rows;
        layer.cols = cols;
        if (!readFloats(f, layer.weight, rows * cols)) return false;
        if (!readFloats(f, layer.bias,   rows))        return false;
    }

    // Optional LayerNorm (tag 0x4C4E0000)
    uint32_t tag = 0;
    if (readU32(f, tag) && tag == 0x4C4E0000u) {
        int32_t dim;
        if (readI32(f, dim) &&
            readFloats(f, m_lnWeight, dim) &&
            readFloats(f, m_lnBias,   dim)) {
            m_hasLN = true;
        }
    }

    m_loaded = true;
    std::cout << "[ValueNet] Loaded " << numLayers << " layers from " << path
              << (m_hasLN ? " (with LayerNorm)" : "") << "\n";
    return true;
}

void ValueNetInference::layerNorm(std::vector<float>& x) const noexcept {
    if (!m_hasLN || m_lnWeight.empty()) return;
    const int n = static_cast<int>(x.size());
    // mean
    float mean = 0.f;
    for (float v : x) mean += v;
    mean /= static_cast<float>(n);
    // variance
    float var = 0.f;
    for (float v : x) { float d = v - mean; var += d * d; }
    var /= static_cast<float>(n);
    const float invstd = 1.f / std::sqrt(var + 1e-5f);
    for (int i = 0; i < n; ++i)
        x[i] = (x[i] - mean) * invstd * m_lnWeight[i] + m_lnBias[i];
}

void ValueNetInference::linear(const Layer& l,
                                const std::vector<float>& x,
                                std::vector<float>& y) noexcept {
    const int rows = l.rows;
    const int cols = l.cols;
    y.resize(static_cast<size_t>(rows));
    const float* W = l.weight.data();
    const float* b = l.bias.data();
    const float* xd = x.data();
    for (int i = 0; i < rows; ++i) {
        float acc = b[i];
        const float* row = W + static_cast<std::ptrdiff_t>(i) * cols;
        for (int j = 0; j < cols; ++j)
            acc += row[j] * xd[j];
        y[i] = acc;
    }
}

void ValueNetInference::relu(std::vector<float>& x) noexcept {
    for (float& v : x) if (v < 0.f) v = 0.f;
}

float ValueNetInference::predict(const float* state) const noexcept {
    if (!m_loaded || m_layers.empty()) return 0.f;

    // Copy input into a working buffer
    std::vector<float> h(state, state + kStateSize);

    // Apply LayerNorm before the first linear layer
    layerNorm(h);

    // Forward through layers with ReLU on all but the last
    std::vector<float> out;
    const size_t nLayers = m_layers.size();
    for (size_t i = 0; i < nLayers; ++i) {
        linear(m_layers[i], h, out);
        if (i + 1 < nLayers)
            relu(out);   // ReLU on hidden layers
        h = std::move(out);
    }

    // Tanh on the scalar output
    float v = h.empty() ? 0.f : h[0];
    return std::tanh(v);
}

} // namespace mtg
