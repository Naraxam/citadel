#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace mtg {

// ── ValueNetInference ─────────────────────────────────────────────────────────
//
// Zero-dependency forward pass for the MLP value network exported by
// training/export_cpp.py.
//
// Architecture (matching training/model.py ValueNet):
//   LayerNorm(268)
//   Linear(268→512) + ReLU
//   Linear(512→512) + ReLU
//   Linear(512→256) + ReLU
//   Linear(256→64)  + ReLU   ← value head
//   Linear(64→1)    + Tanh
//
// All arithmetic uses float32 — fast enough for MCTS leaf evaluation
// (~0.3 ms per call on a modern CPU, comfortably under 30s/turn budget).
//
// Binary format (from export_cpp.py):
//   4B magic "CVNW" | int32 version=1 | int32 state_size | int32 num_layers
//   For each layer: int32 rows | int32 cols | rows*cols float32 W | rows float32 b
//   LayerNorm (tag 0x4C4E0000): int32 dim | dim float32 w | dim float32 b

class ValueNetInference {
public:
    static constexpr int kStateSize = 268;

    // Load weights from a binary file written by export_cpp.py.
    // Returns true on success.  Prints a diagnostic and returns false on failure.
    bool load(const std::string& path);

    // Returns true when a model has been loaded successfully.
    bool loaded() const noexcept { return m_loaded; }

    // Evaluate a game state vector (length kStateSize).
    // Returns a value in [-1, +1] where +1 = "current player is winning".
    // Requires loaded() == true.
    float predict(const float* state) const noexcept;

    // Convenience overload for std::array.
    float predict(const std::array<float, kStateSize>& state) const noexcept {
        return predict(state.data());
    }

private:
    struct Layer {
        int                  rows = 0, cols = 0;
        std::vector<float>   weight;   // [rows * cols], row-major
        std::vector<float>   bias;     // [rows]
    };

    bool                 m_loaded = false;
    std::vector<Layer>   m_layers;
    std::vector<float>   m_lnWeight;   // LayerNorm scale
    std::vector<float>   m_lnBias;     // LayerNorm shift
    bool                 m_hasLN = false;

    // Apply layer-normalisation: y = (x - mean) / sqrt(var + eps) * w + b
    void layerNorm(std::vector<float>& x) const noexcept;
    // Matmul + bias: y[i] = sum_j(W[i,j]*x[j]) + b[i]
    static void linear(const Layer& l,
                       const std::vector<float>& x,
                       std::vector<float>& y) noexcept;
    static void relu(std::vector<float>& x) noexcept;
};

} // namespace mtg
