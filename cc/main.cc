// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "absl/base/thread_annotations.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "cc/constants.h"
#include "cc/dual_net/batching_dual_net.h"
#include "cc/dual_net/factory.h"
#include "cc/dual_net/reloading_dual_net.h"
#include "cc/file/path.h"
#include "cc/file/utils.h"
#include "cc/gtp_player.h"
#include "cc/init.h"
#include "cc/logging.h"
#include "cc/mcts_player.h"
#include "cc/platform/utils.h"
#include "cc/random.h"
#include "cc/sgf.h"
#include "cc/tf_utils.h"
#include "cc/zobrist.h"
#include "gflags/gflags.h"

// Game options flags.
DEFINE_string(mode, "",
              "Mode to run in: \"selfplay\", \"eval\", \"gtp\" or \"puzzle\".");
DEFINE_int32(
    ponder_limit, 0,
    "If non-zero and in GTP mode, the number times of times to perform tree "
    "search while waiting for the opponent to play.");
DEFINE_bool(
    courtesy_pass, false,
    "If true and in GTP mode, we will always pass if the opponent passes.");
DEFINE_double(resign_threshold, -0.999, "Resign threshold.");
DEFINE_double(komi, minigo::kDefaultKomi, "Komi.");
DEFINE_double(disable_resign_pct, 0.1,
              "Fraction of games to disable resignation for.");
DEFINE_uint64(seed, 0,
              "Random seed. Use default value of 0 to use a time-based seed. "
              "This seed is used to control the moves played, not whether a "
              "game has resignation disabled or is a holdout.");
DEFINE_double(holdout_pct, 0.03,
              "Fraction of games to hold out for validation.");

// Tree search flags.
DEFINE_int32(num_readouts, 100,
             "Number of readouts to make during tree search for each move.");
DEFINE_int32(virtual_losses, 8,
             "Number of virtual losses when running tree search.");
DEFINE_bool(inject_noise, true,
            "If true, inject noise into the root position at the start of "
            "each tree search.");
DEFINE_bool(soft_pick, true,
            "If true, choose moves early in the game with a probability "
            "proportional to the number of times visited during tree search. "
            "If false, always play the best move.");
DEFINE_bool(random_symmetry, true,
            "If true, randomly flip & rotate the board features before running "
            "the model and apply the inverse transform to the results.");
DEFINE_double(fpu, 0.0,
             "New children value initialize penaly.\n"
             "child value = parent's value - fpu * color, clamped to [-1, 1].\n"
             "0 is init-to-parent [default], 2.0 is init-to-loss.\n"
             "The name FPU (\"First Play Urgency\") is used for consistency "
             "with Leela though our implentations differ significantly.");

DEFINE_string(flags_path, "",
              "Optional path to load flags from. Flags specified in this file "
              "take priority over command line flags. When running selfplay "
              "with run_forever=true, the flag file is reloaded periodically. "
              "Note that flags_path is different from gflags flagfile, which "
              "is only parsed once on startup.");

// Time control flags.
DEFINE_double(seconds_per_move, 0,
              "If non-zero, the number of seconds to spend thinking about each "
              "move instead of using a fixed number of readouts.");
DEFINE_double(
    time_limit, 0,
    "If non-zero, the maximum amount of time to spend thinking in a game: we "
    "spend seconds_per_move thinking for each move for as many moves as "
    "possible before exponentially decaying the amount of time.");
DEFINE_double(decay_factor, 0.98,
              "If time_limit is non-zero, the decay factor used to shorten the "
              "amount of time spent thinking as the game progresses.");
DEFINE_bool(run_forever, false,
            "When running 'selfplay' mode, whether to run forever.");

// Inference flags.
DEFINE_string(model, "",
              "Path to a minigo model. The format of the model depends on the "
              "inferece engine. For engine=tf, the model should be a GraphDef "
              "proto. For engine=lite, the model should be .tflite "
              "flatbuffer.");
DEFINE_string(model_two, "",
              "When running 'eval' mode, provide a path to a second minigo "
              "model, also serialized as a GraphDef proto.");
DEFINE_int32(parallel_games, 32, "Number of games to play in parallel.");
DEFINE_string(checkpoint_glob, "",
              "A glob to monitor for newly trained models. When a new model is "
              "found, it is loaded and used for further inferences.");

// Output flags.
DEFINE_string(output_dir, "",
              "Output directory. If empty, no examples are written.");
DEFINE_string(holdout_dir, "",
              "Holdout directory. If empty, no examples are written.");
DEFINE_string(output_bigtable, "",
              "Output Bigtable specification, of the form: "
              "project,instance,table. "
              "If empty, no examples are written to Bigtable.");
DEFINE_string(sgf_dir, "",
              "SGF directory for selfplay and puzzles. If empty in selfplay "
              "mode, no SGF is written.");
DEFINE_string(bigtable_tag, "", "Used in Bigtable metadata");

// Self play flags:
//   --inject_noise=true
//   --soft_pick=true
//   --random_symmetery=true
//
// Two player flags:
//   --inject_noise=false
//   --soft_pick=false
//   --random_symmetry=true

namespace minigo {
namespace {

std::string GetOutputName(absl::Time now, size_t i) {
  auto timestamp = absl::ToUnixSeconds(now);
  std::string output_name;
  char hostname[64];
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    std::strncpy(hostname, "unknown", sizeof(hostname));
  }
  return absl::StrCat(timestamp, "-", hostname, "-", i);
}

std::string GetOutputDir(absl::Time now, const std::string& root_dir) {
  auto sub_dirs = absl::FormatTime("%Y-%m-%d-%H", now, absl::UTCTimeZone());
  return file::JoinPath(root_dir, sub_dirs);
}

std::string FormatInferenceInfo(
    const std::vector<MctsPlayer::InferenceInfo>& inferences) {
  std::vector<std::string> parts;
  parts.reserve(inferences.size());
  for (const auto& info : inferences) {
    parts.push_back(absl::StrCat(info.model, "(", info.first_move, ",",
                                 info.last_move, ")"));
  }
  return absl::StrJoin(parts, ", ");
}

void WriteSgf(const std::string& output_dir, const std::string& output_name,
              const MctsPlayer& player_b, const MctsPlayer& player_w,
              bool write_comments) {
  MG_CHECK(file::RecursivelyCreateDir(output_dir));
  MG_CHECK(player_b.history().size() == player_w.history().size());

  bool log_names = player_b.name() != player_w.name();

  std::vector<sgf::MoveWithComment> moves;
  moves.reserve(player_b.history().size());

  for (size_t i = 0; i < player_b.history().size(); ++i) {
    const auto& h = i % 2 == 0 ? player_b.history()[i] : player_w.history()[i];
    const auto& color = h.node->position.to_play();
    std::string comment;
    if (write_comments) {
      if (i == 0) {
        comment = absl::StrCat(
            "Resign Threshold: ", player_b.options().resign_threshold, "\n",
            h.comment);
      } else {
        if (log_names) {
          comment = absl::StrCat(i % 2 == 0 ? player_b.name() : player_w.name(),
                                 "\n", h.comment);
        } else {
          comment = h.comment;
        }
      }
      moves.emplace_back(color, h.c, std::move(comment));
    } else {
      moves.emplace_back(color, h.c, "");
    }
  }

  std::string player_name(file::Basename(FLAGS_model));
  sgf::CreateSgfOptions options;
  options.komi = player_b.options().komi;
  options.result = player_b.result_string();
  options.black_name = player_b.name();
  options.white_name = player_w.name();
  options.game_comment = absl::StrCat(
      "B inferences: ", FormatInferenceInfo(player_b.inferences()), "\n",
      "W inferences: ", FormatInferenceInfo(player_w.inferences()));

  auto sgf_str = sgf::CreateSgfString(moves, options);

  auto output_path = file::JoinPath(output_dir, output_name + ".sgf");
  MG_CHECK(file::WriteFile(output_path, sgf_str));
}

void WriteSgf(const std::string& output_dir, const std::string& output_name,
              const MctsPlayer& player, bool write_comments) {
  WriteSgf(output_dir, output_name, player, player, write_comments);
}

void ParseMctsPlayerOptionsFromFlags(MctsPlayer::Options* options) {
  options->inject_noise = FLAGS_inject_noise;
  options->soft_pick = FLAGS_soft_pick;
  options->random_symmetry = FLAGS_random_symmetry;
  options->value_init_penalty = FLAGS_fpu;
  options->resign_threshold = FLAGS_resign_threshold;
  options->batch_size = FLAGS_virtual_losses;
  options->komi = FLAGS_komi;
  options->random_seed = FLAGS_seed;
  options->num_readouts = FLAGS_num_readouts;
  options->seconds_per_move = FLAGS_seconds_per_move;
  options->time_limit = FLAGS_time_limit;
  options->decay_factor = FLAGS_decay_factor;
}

void LogEndGameInfo(const MctsPlayer& player, absl::Duration game_time) {
  std::cout << player.result_string() << std::endl;
  std::cout << "Playing game: " << absl::ToDoubleSeconds(game_time)
            << std::endl;
  std::cout << "Played moves: " << player.root()->position.n() << std::endl;

  const auto& history = player.history();
  if (history.empty()) {
    return;
  }

  int bleakest_move = 0;
  float q = 0.0;
  if (FindBleakestMove(player, &bleakest_move, &q)) {
    std::cout << "Bleakest eval: move=" << bleakest_move << " Q=" << q
              << std::endl;
  }

  // If resignation is disabled, check to see if the first time Q_perspective
  // crossed the resign_threshold the eventual winner of the game would have
  // resigned. Note that we only check for the first resignation: if the
  // winner would have incorrectly resigned AFTER the loser would have
  // resigned on an earlier move, this is not counted as a bad resignation for
  // the winner (since the game would have ended after the loser's initial
  // resignation).
  float result = player.result();
  if (!player.options().resign_enabled) {
    for (size_t i = 0; i < history.size(); ++i) {
      if (history[i].node->Q_perspective() <
          player.options().resign_threshold) {
        if ((history[i].node->Q() < 0) != (result < 0)) {
          std::cout << "Bad resign: move=" << i << " Q=" << history[i].node->Q()
                    << std::endl;
        }
        break;
      }
    }
  }
}

class SelfPlayer {
 public:
  void Run() {
    auto start_time = absl::Now();
    {
      absl::MutexLock lock(&mutex_);
      dual_net_factory_ = NewDualNetFactory();
      // If the model path contains a pattern, wrap the implementation factory
      // in a ReloadingDualNetFactory to automatically reload the latest model
      // that matches the pattern.
      if (FLAGS_model.find("%d") != std::string::npos) {
        dual_net_factory_ = absl::make_unique<ReloadingDualNetFactory>(
            std::move(dual_net_factory_), absl::Seconds(3));
      }
      // Note: it's more efficient to perform the reload wrapping before the
      // batch wrapping because this way, we only need to reload the single
      // implementation DualNet when a new model is found. If we performed batch
      // wrapping before reload wrapping, the reload code would need to update
      // all the BatchingDualNet wrappers.
      if (FLAGS_parallel_games > 1) {
        dual_net_factory_ =
            NewBatchingDualNetFactory(std::move(dual_net_factory_));
      }
    }
    for (int i = 0; i < FLAGS_parallel_games; ++i) {
      threads_.emplace_back(std::bind(&SelfPlayer::ThreadRun, this, i));
    }
    for (auto& t : threads_) {
      t.join();
    }
    MG_LOG(INFO) << "Played " << FLAGS_parallel_games << " games, total time "
                 << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec.";
  }

 private:
  // Struct that holds the options for a game. Each thread has its own
  // GameOptions instance, which are initialized with the SelfPlayer's mutex
  // held. This allows us to safely update the command line arguments from a
  // flag file without causing any race conditions.
  struct GameOptions {
    void Init(int thread_id, Random* rnd) {
      ParseMctsPlayerOptionsFromFlags(&player_options);
      player_options.verbose = thread_id == 0;
      // If an random seed was explicitly specified, make sure we use a
      // different seed for each thread.
      if (player_options.random_seed != 0) {
        player_options.random_seed += 1299283 * thread_id;
      }
      player_options.resign_enabled = (*rnd)() >= FLAGS_disable_resign_pct;

      run_forever = FLAGS_run_forever;
      holdout_pct = FLAGS_holdout_pct;
      output_dir = FLAGS_output_dir;
      holdout_dir = FLAGS_holdout_dir;
      sgf_dir = FLAGS_sgf_dir;
    }

    MctsPlayer::Options player_options;
    bool run_forever;
    float holdout_pct;
    std::string output_dir;
    std::string holdout_dir;
    std::string sgf_dir;
  };

  void ThreadRun(int thread_id) {
    // Only print the board using ANSI colors if stderr is sent to the
    // terminal.
    const bool use_ansi_colors = FdSupportsAnsiColors(fileno(stderr));

    GameOptions game_options;
    std::vector<std::string> bigtable_spec =
        absl::StrSplit(FLAGS_output_bigtable, ',');
    bool use_bigtable = bigtable_spec.size() == 3;
    if (!FLAGS_output_bigtable.empty() && !use_bigtable) {
      MG_LOG(FATAL)
          << "Bigtable output must be of the form: project,instance,table";
      return;
    }

    do {
      std::unique_ptr<MctsPlayer> player;

      {
        absl::MutexLock lock(&mutex_);
        auto old_model = FLAGS_model;
        MaybeReloadFlags();
        MG_CHECK(old_model == FLAGS_model)
            << "Manually changing the model during selfplay is not supported.";
        game_options.Init(thread_id, &rnd_);
        player = absl::make_unique<MctsPlayer>(
            dual_net_factory_->NewDualNet(FLAGS_model),
            game_options.player_options);
      }

      // Play the game.
      auto start_time = absl::Now();
      dual_net_factory_->StartGame(player->network(), player->network());
      while (!player->root()->game_over() && !player->root()->at_move_limit()) {
        auto move = player->SuggestMove();
        if (player->options().verbose) {
          const auto& position = player->root()->position;
          MG_LOG(INFO) << player->root()->position.ToPrettyString(
              use_ansi_colors);
          MG_LOG(INFO) << "Move: " << position.n()
                       << " Captures X: " << position.num_captures()[0]
                       << " O: " << position.num_captures()[1];
          MG_LOG(INFO) << player->root()->Describe();
        }
        MG_CHECK(player->PlayMove(move));
      }
      dual_net_factory_->EndGame(player->network(), player->network());

      {
        // Log the end game info with the shared mutex held to prevent the
        // outputs from multiple threads being interleaved.
        absl::MutexLock lock(&mutex_);
        LogEndGameInfo(*player, absl::Now() - start_time);
      }

      // Write the outputs.
      auto now = absl::Now();
      auto output_name = GetOutputName(now, thread_id);

      bool is_holdout;
      {
        absl::MutexLock lock(&mutex_);
        is_holdout = rnd_() < game_options.holdout_pct;
      }
      auto example_dir =
          is_holdout ? game_options.holdout_dir : game_options.output_dir;
      if (!example_dir.empty()) {
        tf_utils::WriteGameExamples(GetOutputDir(now, example_dir), output_name,
                                    *player);
      }
      if (use_bigtable) {
        const auto& gcp_project_name = bigtable_spec[0];
        const auto& instance_name = bigtable_spec[1];
        const auto& table_name = bigtable_spec[2];
        tf_utils::WriteGameExamples(gcp_project_name, instance_name, table_name,
                                    *player);
      }

      if (!game_options.sgf_dir.empty()) {
        WriteSgf(
            GetOutputDir(now, file::JoinPath(game_options.sgf_dir, "clean")),
            output_name, *player, false);
        WriteSgf(
            GetOutputDir(now, file::JoinPath(game_options.sgf_dir, "full")),
            output_name, *player, true);
      }
    } while (game_options.run_forever);

    MG_LOG(INFO) << "Thread " << thread_id << " stopping";
  }

  void MaybeReloadFlags() EXCLUSIVE_LOCKS_REQUIRED(&mutex_) {
    if (FLAGS_flags_path.empty()) {
      return;
    }
    uint64_t new_flags_timestamp;
    MG_CHECK(file::GetModTime(FLAGS_flags_path, &new_flags_timestamp));
    bool skip = new_flags_timestamp == flags_timestamp_;
    MG_LOG(INFO) << "flagfile:" << FLAGS_flags_path
                 << " old_ts:" << absl::FromUnixMicros(flags_timestamp_)
                 << " new_ts:" << absl::FromUnixMicros(new_flags_timestamp)
                 << (skip ? " skipping" : "");
    if (skip) {
      return;
    }

    flags_timestamp_ = new_flags_timestamp;
    std::string contents;
    MG_CHECK(file::ReadFile(FLAGS_flags_path, &contents));

    std::vector<std::string> lines =
        absl::StrSplit(contents, '\n', absl::SkipEmpty());
    MG_LOG(INFO) << " loaded flags:" << absl::StrJoin(lines, " ");

    for (absl::string_view line : lines) {
      std::pair<absl::string_view, absl::string_view> line_comment =
          absl::StrSplit(line, absl::MaxSplits('#', 1));
      line = absl::StripAsciiWhitespace(line_comment.first);
      if (line.empty()) {
        continue;
      }
      MG_CHECK(line.length() > 2 && line[0] == '-' && line[1] == '-') << line;
      std::pair<std::string, std::string> flag_value =
          absl::StrSplit(line, absl::MaxSplits('=', 1));
      flag_value.first = flag_value.first.substr(2);
      MG_LOG(INFO) << "Setting command line flag: --" << flag_value.first << "="
                   << flag_value.second;
      gflags::SetCommandLineOption(flag_value.first.c_str(),
                                   flag_value.second.c_str());
    }
  }

  absl::Mutex mutex_;
  std::unique_ptr<DualNetFactory> dual_net_factory_ GUARDED_BY(&mutex_);
  Random rnd_ GUARDED_BY(&mutex_);
  std::vector<std::thread> threads_;
  uint64_t flags_timestamp_ = 0;
};

class Evaluator {
  struct Model {
    explicit Model(const std::string& path)
        : path(path), name(file::Stem(path)), black_wins(0), white_wins(0) {}
    const std::string path;
    const std::string name;
    std::atomic<int> black_wins;
    std::atomic<int> white_wins;
  };

 public:
  void Run() {
    auto start_time = absl::Now();
    auto model_factory = NewDualNetFactory();
    if (FLAGS_parallel_games > 1) {
      model_factory = NewBatchingDualNetFactory(std::move(model_factory));
    }

    Model model_a(FLAGS_model);
    Model model_b(FLAGS_model_two);

    MG_LOG(INFO) << "DualNet factories created from " << FLAGS_model
                 << "\n  and " << FLAGS_model_two << " in "
                 << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec.";

    ParseMctsPlayerOptionsFromFlags(&options_);
    options_.inject_noise = false;
    options_.soft_pick = false;
    options_.random_symmetry = true;

    int num_games = FLAGS_parallel_games;
    for (int thread_id = 0; thread_id < num_games; ++thread_id) {
      bool swap_models = (thread_id & 1) != 0;
      threads_.emplace_back(std::bind(&Evaluator::ThreadRun, this, thread_id,
                                      model_factory.get(),
                                      swap_models ? &model_a : &model_b,
                                      swap_models ? &model_b : &model_a));
    }
    for (auto& t : threads_) {
      t.join();
    }

    MG_LOG(INFO) << "Evaluated " << num_games << " games, total time "
                 << (absl::Now() - start_time);

    auto name_length = std::max(model_a.name.size(), model_b.name.size());
    auto format_name = [&](const std::string& name) {
      return absl::StrFormat("%-*s", name_length, name);
    };
    auto format_wins = [&](int wins) {
      return absl::StrFormat(" %5d %6.2f%%", wins, wins * 100.0f / num_games);
    };
    auto print_result = [&](const Model& model) {
      MG_LOG(INFO) << format_name(model.name)
                   << format_wins(model.black_wins + model.white_wins)
                   << format_wins(model.black_wins)
                   << format_wins(model.white_wins);
    };

    MG_LOG(INFO) << format_name("Wins")
                 << "        Total         Black         White";
    print_result(model_a);
    print_result(model_b);
    MG_LOG(INFO) << format_name("") << "              "
                 << format_wins(model_a.black_wins + model_b.black_wins)
                 << format_wins(model_a.white_wins + model_b.white_wins);
  }

 private:
  void ThreadRun(int thread_id, DualNetFactory* model_factory,
                 Model* black_model, Model* white_model) {
    // The player and other_player reference this pointer.
    std::unique_ptr<DualNet> dual_net;

    std::vector<std::string> bigtable_spec =
        absl::StrSplit(FLAGS_output_bigtable, ',');
    bool use_bigtable = bigtable_spec.size() == 3;
    if (!FLAGS_output_bigtable.empty() && !use_bigtable) {
      MG_LOG(FATAL)
          << "Bigtable output must be of the form: project,instance,table";
      return;
    }

    auto player_options = options_;
    // If an random seed was explicitly specified, make sure we use a
    // different seed for each thread.
    if (player_options.random_seed != 0) {
      player_options.random_seed += 1299283 * thread_id;
    }

    const bool verbose = thread_id == 0;
    player_options.verbose = verbose;
    player_options.name = black_model->name;
    auto black = absl::make_unique<MctsPlayer>(
        model_factory->NewDualNet(black_model->path), player_options);

    player_options.verbose = false;
    player_options.name = white_model->name;
    auto white = absl::make_unique<MctsPlayer>(
        model_factory->NewDualNet(white_model->path), player_options);

    auto* curr_player = black.get();
    auto* next_player = white.get();
    model_factory->StartGame(curr_player->network(), next_player->network());
    while (!curr_player->root()->game_over() &&
           !curr_player->root()->at_move_limit()) {
      auto move = curr_player->SuggestMove();
      if (curr_player->options().verbose) {
        std::cerr << curr_player->root()->Describe() << "\n";
      }
      curr_player->PlayMove(move);
      next_player->PlayMove(move);
      if (curr_player->options().verbose) {
        MG_LOG(INFO) << curr_player->root()->position.ToPrettyString();
      }
      std::swap(curr_player, next_player);
    }
    model_factory->EndGame(curr_player->network(), next_player->network());

    MG_CHECK(curr_player->result() == next_player->result());
    if (curr_player->result() > 0) {
      ++black_model->black_wins;
    }
    if (curr_player->result() < 0) {
      ++white_model->white_wins;
    }

    if (verbose) {
      MG_LOG(INFO) << black->result_string();
      MG_LOG(INFO) << "Black was: " << black->name();
    }

    // Write SGF.
    std::string output_name = "NO_SGF_SAVED";
    if (!FLAGS_sgf_dir.empty()) {
      output_name = absl::StrCat(GetOutputName(absl::Now(), thread_id), "-",
                                 black->name(), "-", white->name());
      WriteSgf(FLAGS_sgf_dir, output_name, *black, *white, true);
    }

    if (use_bigtable) {
      const auto& gcp_project_name = bigtable_spec[0];
      const auto& instance_name = bigtable_spec[1];
      const auto& table_name = bigtable_spec[2];
      tf_utils::WriteEvalRecord(gcp_project_name, instance_name, table_name,
                                *curr_player, black->name(), white->name(),
                                output_name, FLAGS_bigtable_tag);
    }

    MG_LOG(INFO) << "Thread " << thread_id << " stopping";
  }

  MctsPlayer::Options options_;
  std::vector<std::thread> threads_;
};

void SelfPlay() {
  SelfPlayer player;
  player.Run();
}

void Eval() {
  Evaluator evaluator;
  evaluator.Run();
}

void Gtp() {
  GtpPlayer::Options options;
  ParseMctsPlayerOptionsFromFlags(&options);

  options.name = absl::StrCat("minigo-", file::Basename(FLAGS_model));
  options.ponder_limit = FLAGS_ponder_limit;
  options.courtesy_pass = FLAGS_courtesy_pass;
  auto model_factory = NewDualNetFactory();
  auto player = absl::make_unique<GtpPlayer>(
      model_factory->NewDualNet(FLAGS_model), options);
  model_factory->StartGame(player->network(), player->network());
  player->Run();
  model_factory->EndGame(player->network(), player->network());
}

void Puzzle() {
  auto start_time = absl::Now();

  auto model_factory = NewDualNetFactory();
  MG_LOG(INFO) << "DualNet factory created from " << FLAGS_model << " in "
               << absl::ToDoubleSeconds(absl::Now() - start_time) << " sec.";

  MctsPlayer::Options options;
  ParseMctsPlayerOptionsFromFlags(&options);
  options.verbose = false;

  std::atomic<size_t> total_moves(0);
  std::atomic<size_t> correct_moves(0);

  std::vector<std::thread> threads;
  std::vector<std::string> basenames;
  MG_CHECK(file::ListDir(FLAGS_sgf_dir, &basenames));
  for (const auto& basename : basenames) {
    if (!absl::EndsWith(basename, ".sgf")) {
      continue;
    }
    threads.emplace_back([&]() {
      // Read the main line from the SGF.
      auto path = file::JoinPath(FLAGS_sgf_dir, basename);
      std::string contents;
      MG_CHECK(file::ReadFile(path, &contents));
      sgf::Ast ast;
      MG_CHECK(ast.Parse(contents));
      auto trees = GetTrees(ast);
      MG_CHECK(!trees.empty());
      auto moves = trees[0]->ExtractMainLine();

      total_moves += moves.size();

      // Create player.
      auto player = absl::make_unique<MctsPlayer>(
          model_factory->NewDualNet(FLAGS_model), options);
      model_factory->StartGame(player->network(), player->network());

      // Play through each game. For each position in the game, compare the
      // model's suggested move to the actual move played in the game.
      for (size_t move_to_predict = 0; move_to_predict < moves.size();
           ++move_to_predict) {
        // Reset the game and play up to the position to be tested.
        player->NewGame();
        for (size_t i = 0; i < move_to_predict; ++i) {
          player->PlayMove(moves[i].c);
        }

        // Check if we predict the move that was played.
        auto expected_move = moves[move_to_predict].c;
        auto actual_move = player->SuggestMove();
        if (actual_move == expected_move) {
          ++correct_moves;
        }
      }
      model_factory->EndGame(player->network(), player->network());
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  MG_LOG(INFO) << absl::StreamFormat(
      "Solved %d of %d puzzles (%3.1f%%), total time %f sec.", correct_moves,
      total_moves, correct_moves * 100.0f / total_moves,
      absl::ToDoubleSeconds(absl::Now() - start_time));
}

}  // namespace
}  // namespace minigo

int main(int argc, char* argv[]) {
  minigo::Init(&argc, &argv);

  minigo::zobrist::Init(FLAGS_seed * 614944751);

  if (FLAGS_mode == "selfplay") {
    minigo::SelfPlay();
  } else if (FLAGS_mode == "eval") {
    minigo::Eval();
  } else if (FLAGS_mode == "gtp") {
    minigo::Gtp();
  } else if (FLAGS_mode == "puzzle") {
    minigo::Puzzle();
  } else {
    MG_LOG(FATAL) << "Unrecognized mode \"" << FLAGS_mode << "\"";
  }

  return 0;
}
