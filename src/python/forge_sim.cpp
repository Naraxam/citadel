#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
#include <fstream>

#include "../core/db/CardDb.h"
#include "../sim/GameRunner.h"

namespace py = pybind11;
using namespace mtg;

static py::array_t<float> episodeStatesToNumpy(const std::vector<GameRunner::StateVec>& states) {
    size_t n = states.size();
    py::array_t<float> arr({n, static_cast<size_t>(GameRunner::kStateSize)});
    if (n == 0) return arr;
    auto buf = arr.mutable_unchecked<2>();
    for (size_t i = 0; i < n; ++i)
        for (int j = 0; j < GameRunner::kStateSize; ++j)
            buf(i, j) = states[i][j];
    return arr;
}

PYBIND11_MODULE(citadel_sim, m) {
    m.doc() = "Citadel MTG simulation engine — pybind11 interface";

    // ── CardDb ────────────────────────────────────────────────────────────────
    py::class_<CardDb>(m, "CardDb")
        .def(py::init<>())
        .def("load_from_zip",       &CardDb::loadFromZip,
             py::arg("zip_path"),
             "Load cards from Forge's cardsfolder.zip archive.")
        .def("load_from_directory", &CardDb::loadFromDirectory,
             py::arg("dir_path"),
             "Recursively load all card .txt files under dir_path.")
        .def("wire_back_faces",     &CardDb::wireBackFaces,
             "Resolve back-face references after loading (call once).")
        .def("__len__",             &CardDb::size)
        .def("empty",               &CardDb::empty);

    // ── Episode ───────────────────────────────────────────────────────────────
    py::class_<GameRunner::Episode>(m, "Episode")
        .def_readonly("winner", &GameRunner::Episode::winner,
                      "0, 1, or -1 (draw / timeout)")
        .def_readonly("turns",  &GameRunner::Episode::turns)
        .def_property_readonly("states", [](const GameRunner::Episode& ep) {
            return episodeStatesToNumpy(ep.states);
        }, "float32 numpy array of shape (N, 128) — board state per decision step.")
        .def_property_readonly("actions", [](const GameRunner::Episode& ep) {
            return py::array_t<int>(
                {ep.actions.size()},
                {sizeof(int)},
                ep.actions.data());
        }, "int32 numpy array of shape (N,) — action index per step.")
        .def_property_readonly("rewards", [](const GameRunner::Episode& ep) {
            return py::array_t<float>(
                {ep.rewards.size()},
                {sizeof(float)},
                ep.rewards.data());
        }, "float32 numpy array of shape (N,) — terminal reward per step.")
        .def_property_readonly("policy", [](const GameRunner::Episode& ep) {
            // Return a ragged list of 1-D float32 numpy arrays — one per MCTS decision step.
            py::list result;
            for (const auto& v : ep.policy) {
                auto arr = py::array_t<float>({v.size()}, {sizeof(float)}, v.data());
                result.append(arr);
            }
            return result;
        }, "list of float32 numpy arrays — MCTS visit distribution per decision step "
           "(empty list when MCTS is disabled).");

    // ── GameRunner ────────────────────────────────────────────────────────────
    // Note: db must outlive the GameRunner — py::keep_alive enforces this.
    py::class_<GameRunner>(m, "GameRunner")
        .def(py::init<const CardDb&, const std::string&, const std::string&>(),
             py::arg("db"), py::arg("deck0_path"), py::arg("deck1_path"),
             py::keep_alive<1, 2>())
        .def("run_episode", [](GameRunner& self, int maxTurns) {
            py::gil_scoped_release release;
            return self.runEpisode(maxTurns);
        }, py::arg("max_turns") = 200,
           "Run one game and return an Episode. Releases the GIL for parallel workers.")
        .def("run_game", [](GameRunner& self, int maxTurns) {
            py::gil_scoped_release release;
            return self.runGame(maxTurns);
        }, py::arg("max_turns") = 200,
           "Run one game, return winner (0/1) or -1. Releases the GIL.")
        .def("last_winner",   &GameRunner::lastWinner)
        .def("last_turns",    &GameRunner::lastTurns)
        .def("set_debug_log",
             [](GameRunner& self, const std::string& path) {
                 // Open a persistent log stream; intentionally leaked (game-lifetime).
                 // Pass empty string to disable.
                 if (path.empty()) { self.setDebugLog(nullptr); return; }
                 auto* f = new std::ofstream(path, std::ios::out | std::ios::trunc);
                 if (*f) self.setDebugLog(f);
             },
             py::arg("path") = "",
             "Enable per-turn debug logging to the given file path (for crash diagnosis).")
        .def("enable_mcts",
             [](GameRunner& self, bool on, int iterations, float c_puct) {
                 MctsConfig cfg;
                 cfg.iterations = iterations;
                 cfg.c_puct     = c_puct;
                 self.setMcts(on, cfg);
             },
             py::arg("on"), py::arg("iterations") = 200, py::arg("c_puct") = 1.5f,
             "Enable/disable MCTS for self-play. "
             "iterations: simulations per decision. c_puct: exploration constant.")
        .def("set_value_fn",
             [](GameRunner& self, py::object fn) {
                 if (fn.is_none()) {
                     self.setValueFn({});
                     return;
                 }
                 // Wrap the Python callable as a C++ ValueFn.
                 // The lambda re-acquires the GIL before calling into Python.
                 // fn is captured by value (py::object holds a reference).
                 self.setValueFn([fn](const GameState& gs, uint8_t pid) -> float {
                     py::gil_scoped_acquire acquire;
                     auto sv = GameRunner::encodeState(gs, pid);
                     // Build a numpy array view over the stack-allocated StateVec.
                     // Copy into a new array so lifetime is safe across the call.
                     py::array_t<float> arr(GameRunner::kStateSize);
                     std::copy(sv.begin(), sv.end(), arr.mutable_data());
                     py::object result = fn(arr);
                     return result.cast<float>();
                 });
             },
             py::arg("fn"),
             "Set a Python callable as the neural-net value function for MCTS.\n"
             "Signature: fn(state: np.ndarray[float32, shape=(STATE_SIZE,)]) -> float.\n"
             "Pass None to revert to the built-in heuristic evaluator.\n"
             "The callable is invoked with GIL held; use a batched/session-level\n"
             "model (e.g. ONNX Runtime InferenceSession) for best performance.")
        .def("set_policy_fn",
             [](GameRunner& self, py::object fn) {
                 if (fn.is_none()) { self.setPolicyFn({}); return; }
                 // PolicyFn: (GameState, playerId, numActions) -> vector<float>
                 // Python callable receives (state_array, num_actions) -> list[float]
                 self.setPolicyFn([fn](const GameState& gs, uint8_t pid, int numActions) -> std::vector<float> {
                     py::gil_scoped_acquire acquire;
                     auto sv = GameRunner::encodeState(gs, pid);
                     py::array_t<float> arr(GameRunner::kStateSize);
                     std::copy(sv.begin(), sv.end(), arr.mutable_data());
                     py::object result = fn(arr, numActions);
                     return result.cast<std::vector<float>>();
                 });
             },
             py::arg("fn"),
             "Set a Python policy prior function for MCTS action selection.\n"
             "Signature: fn(state: np.ndarray, num_actions: int) -> list[float].\n"
             "Should return a softmax probability vector of length num_actions.\n"
             "Pass None to revert to uniform priors.")
        .def("set_value_fn_p0",
             [](GameRunner& self, py::object fn) {
                 if (fn.is_none()) { self.setValueFnP0({}); return; }
                 self.setValueFnP0([fn](const GameState& gs, uint8_t pid) -> float {
                     py::gil_scoped_acquire acquire;
                     auto sv = GameRunner::encodeState(gs, pid);
                     py::array_t<float> arr(GameRunner::kStateSize);
                     std::copy(sv.begin(), sv.end(), arr.mutable_data());
                     return fn(arr).cast<float>();
                 });
             },
             py::arg("fn"),
             "Set a per-player value function for P0 only (used for ELO evaluation).")
        .def("set_value_fn_p1",
             [](GameRunner& self, py::object fn) {
                 if (fn.is_none()) { self.setValueFnP1({}); return; }
                 self.setValueFnP1([fn](const GameState& gs, uint8_t pid) -> float {
                     py::gil_scoped_acquire acquire;
                     auto sv = GameRunner::encodeState(gs, pid);
                     py::array_t<float> arr(GameRunner::kStateSize);
                     std::copy(sv.begin(), sv.end(), arr.mutable_data());
                     return fn(arr).cast<float>();
                 });
             },
             py::arg("fn"),
             "Set a per-player value function for P1 only (used for ELO evaluation).")
        .def("set_batch_value_fn",
             [](GameRunner& self, py::object fn) {
                 if (fn.is_none()) { self.setBatchValueFn({}); return; }
                 // fn signature: fn(states: np.ndarray[float32, (N, STATE_SIZE)]) -> list[float]
                 self.setBatchValueFn([fn](
                         const std::vector<std::pair<const GameState*, uint8_t>>& batch)
                         -> std::vector<float> {
                     py::gil_scoped_acquire acquire;
                     int N = static_cast<int>(batch.size());
                     int S = GameRunner::kStateSize;
                     py::array_t<float> arr({N, S});
                     auto buf = arr.mutable_unchecked<2>();
                     for (int i = 0; i < N; ++i) {
                         auto sv = GameRunner::encodeState(*batch[static_cast<size_t>(i)].first,
                                                            batch[static_cast<size_t>(i)].second);
                         for (int j = 0; j < S; ++j) buf(i, j) = sv[static_cast<size_t>(j)];
                     }
                     py::list result = fn(arr).cast<py::list>();
                     std::vector<float> out;
                     out.reserve(static_cast<size_t>(N));
                     for (auto& v : result) out.push_back(v.cast<float>());
                     return out;
                 });
             },
             py::arg("fn"),
             "Set a GPU-batched value function (fn(states: np.ndarray[N,STATE_SIZE]) -> list[float]).\n"
             "MCTS will collect up to 8 leaves and call this once per batch, enabling a single\n"
             "GPU forward pass instead of N sequential calls.");

    m.attr("STATE_SIZE") = GameRunner::kStateSize;
}
