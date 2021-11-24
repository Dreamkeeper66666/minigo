// Copyright 2019 Google LLC
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

#include "cc/minigui_gtp_client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <utility>
#include <mutex>
#include <condition_variable>


#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/time/clock.h"
#include "cc/constants.h"
#include "cc/file/utils.h"
#include "cc/json.h"
#include "cc/logging.h"
#include "cc/sgf.h"

namespace minigo {

namespace {

// Returns the Q of the best legal child move for the node.
// Returns the node's Q if there are no legal moves.
float GetBestMoveQ(const MctsNode* node) {
  float scale = node->position.to_play() == Color::kBlack ? 1 : -1;
  float bestChildQ = node->Q();
  for (int i = 0; i < kNumMoves; ++i) {
    if (!node->position.legal_move(i)) {
      continue;
    }
    if (node->child_Q(i) * scale > bestChildQ * scale) {
      bestChildQ = node->child_Q(i);
    }
  }
  return bestChildQ;
}

}  // namespace

MiniguiGtpClient::MiniguiGtpClient(
    std::string device,
    std::shared_ptr<ThreadSafeInferenceCache> inference_cache,
    const std::string& model_path, const Game::Options& game_options,
    const MctsPlayer::Options& player_options,
    const GtpClient::Options& client_options)
    : GtpClient(std::move(device), inference_cache, model_path, game_options,
                player_options, client_options) {
  RegisterCmd("echo", &MiniguiGtpClient::HandleEcho);
  RegisterCmd("genmove", &MiniguiGtpClient::HandleGenmove);
  RegisterCmd("lz-analyze", &MiniguiGtpClient::HandleAnalyze);
  RegisterCmd("play", &MiniguiGtpClient::HandlePlay);
  RegisterCmd("report_search_interval",
              &MiniguiGtpClient::HandleReportSearchInterval);
  RegisterCmd("select_position", &MiniguiGtpClient::HandleSelectPosition);
  RegisterCmd("winrate_evals", &MiniguiGtpClient::HandleWinrateEvals);

  player_->SetTreeSearchCallback(
      std::bind(&MiniguiGtpClient::TreeSearchCb, this, std::placeholders::_1));

  variation_tree_ = absl::make_unique<VariationTree>();

  int num_workers = 16;
  int num_win_rate_evals = 8;
  auto worker_options = player_options;
  worker_options.virtual_losses = 1;
  win_rate_evaluator_ = absl::make_unique<WinRateEvaluator>(
      num_workers, num_win_rate_evals, device_, inference_cache, model_path,
      game_options, worker_options);
}

MiniguiGtpClient::~MiniguiGtpClient() = default;

void MiniguiGtpClient::NewGame() {
  GtpClient::NewGame();
  variation_tree_ = absl::make_unique<VariationTree>();
  //ReportRootPosition();
  RefreshPendingWinRateEvals();
}

void MiniguiGtpClient::Ponder() {
  if (win_rate_evaluator_->all_nodes_have_at_least_one_read() && !player_->stop_tree_search_) {
    GtpClient::Ponder();
  }
  win_rate_evaluator_->EvalNodes();
}

GtpClient::Response MiniguiGtpClient::HandleCmd(const std::string& line) {
  auto response = GtpClient::HandleCmd(line);
  // Write __GTP_CMD_DONE__ to stderr to signify that handling a GTP command is
  // done. The Minigui Python server waits for this magic string before it
  // consumes the output of each GTP command. This keeps the outputs written to
  // stderr and stdout synchronized so that all data written to stderr while
  // processing a GTP command is consumed before the GTP result written to
  // stdout.
  //MG_LOG(INFO) << "__GTP_CMD_DONE__";
  return response;
}


GtpClient::Response MiniguiGtpClient::HandleEcho(CmdArgs args) {
  return Response::Ok(absl::StrJoin(args, " "));
}

GtpClient::Response MiniguiGtpClient::HandleGenmove(CmdArgs args) {
  ReportSearchStatus(nullptr, true);
  //auto response = GtpClient::HandleGenmove(args);
  //if (response.ok) {
   // variation_tree_->PlayMove(player_->root()->move);
    //ReportRootPosition();
  //}
  RefreshPendingWinRateEvals();
  return  Response::Ok();
}




GtpClient::Response MiniguiGtpClient::HandlePlay(CmdArgs args) {
  auto response = GtpClient::HandlePlay(args);
  
  if (response.ok) {
    variation_tree_->PlayMove(player_->root()->move);
    //ReportRootPosition();
  }
  RefreshPendingWinRateEvals();
  return response;
}

GtpClient::Response MiniguiGtpClient::HandleReportSearchInterval(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  int x;
  if (!absl::SimpleAtoi(args[0], &x) || x < 0) {
    return Response::Error("couldn't parse ", args[0], " as an integer >= 0");
  }
  report_search_interval_ = absl::Milliseconds(x);

  return Response::Ok();
}

GtpClient::Response MiniguiGtpClient::HandleAnalyze(CmdArgs args) {

  std::cout << "=\n";

  int x;
  if (!absl::SimpleAtoi(args[0], &x) || x < 0) {
    return Response::Error("couldn't parse ", args[0], " as an integer >= 0");
  }
  report_search_interval_ = absl::Milliseconds(x);
    

    std::thread th_analyze([this]() {
            ReportSearchStatus(nullptr, true);});
    th_analyze.detach();

  //ReportSearchStatus(nullptr, true);
  //auto response = GtpClient::HandleGenmove(args);
  //if (response.ok) {
   // variation_tree_->PlayMove(player_->root()->move);
    //ReportRootPosition();
  //}
  //RefreshPendingWinRateEvals();

  return  Response::Ok();
}


GtpClient::Response MiniguiGtpClient::HandleSelectPosition(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  if (!variation_tree_->SelectNode(std::string(args[0]))) {
    return Response::Error("unknown position id");
  }

  player_->NewGame();
  for (auto c : variation_tree_->current_node()->GetVariation()) {
    MG_CHECK(player_->PlayMove(c));
  }

  RefreshPendingWinRateEvals();

  return Response::Ok();
}

GtpClient::Response MiniguiGtpClient::HandleWinrateEvals(CmdArgs args) {
  int num_reads;
  if (!absl::SimpleAtoi(args[1], &num_reads) || num_reads < 0) {
    return Response::Error("invalid num_reads");
  }
  win_rate_evaluator_->SetNumEvalReads(num_reads);
  RefreshPendingWinRateEvals();
  return Response::Ok();
}

GtpClient::Response MiniguiGtpClient::ReplaySgf(
    const sgf::Collection& collection) {
  // Traverse the SGF's game trees, loading them into the backend & running
  // inference on the positions in batches.
  std::function<Response(const sgf::Tree&)> traverse =
      [&](const sgf::Tree& tree) {
        const auto* original_root = player_->root();
        // Play moves for all nodes in this tree.
        for (const auto& node : tree.nodes) {
          if (node->move.c == Coord::kInvalid) {
            // Skip nodes that don't contain a move, appending any comments to
            // the current position.
            absl::StrAppend(&variation_tree_->current_node()->comment,
                            node->GetCommentAndProperties());
            continue;
          }

          if (node->move.color != player_->root()->position.to_play()) {
            // The move color is different than expected. Play a pass move to
            // flip the colors.
            if (player_->root()->move == Coord::kPass) {
              auto expected = ColorToCode(player_->root()->position.to_play());
              auto actual = node->move.ToSgf();
              MG_LOG(ERROR)
                  << "expected move by " << expected << ", got " << actual
                  << " but can't play an intermediate pass because the"
                  << " previous move was also a pass";
              return Response::Error("cannot load file");
            }
            MG_LOG(WARNING) << "Inserting pass move";
            MG_CHECK(player_->PlayMove(Coord::kPass));
            variation_tree_->PlayMove(Coord::kPass);
            ReportRootPosition();
          }

          if (!player_->PlayMove(node->move.c)) {
            MG_LOG(ERROR) << "error playing " << node->move.ToSgf();
            return Response::Error("cannot load file");
          }
          variation_tree_->PlayMove(node->move.c);
          variation_tree_->current_node()->comment =
              node->GetCommentAndProperties();
          ReportRootPosition();
        }

        // Traverse all sub-trees.
        for (const auto& sub_tree : tree.sub_trees) {
          auto response = traverse(*sub_tree);
          if (!response.ok) {
            return response;
          }
        }

        // Undo the moves in this tree before returning.
        while (player_->root() != original_root) {
          player_->UndoMove();
          variation_tree_->GoToParent();
        }
        return Response::Ok();
      };

  for (const auto& tree : collection.trees) {
    auto response = traverse(*tree);
    if (!response.ok) {
      return response;
    }
  }

  // Play the main line.
  player_->NewGame();
  variation_tree_->GoToStart();

  // The root node of the SGF is created by NewGame, before the SGF is loaded:
  // explicitly update the root node comment.
  if (!variation_tree_->current_node()->comment.empty()) {
    nlohmann::json j = {
        {"id", variation_tree_->current_node()->id},
        {"comment", variation_tree_->current_node()->comment},
    };
    MG_LOG(INFO) << "mg-update: " << j.dump();
  }

  if (!collection.trees.empty()) {
    for (const auto& move : collection.trees[0]->ExtractMainLine()) {
      // We already validated that all the moves could be played in traverse(),
      // so if PlayMove fails here, something has gone seriously awry.
      MG_CHECK(player_->PlayMove(move.c));
      variation_tree_->PlayMove(move.c);
    }
    RefreshPendingWinRateEvals();
    ReportRootPosition();
  }

  return Response::Ok();
}

void MiniguiGtpClient::ReportSearchStatus(const MctsNode* leaf,
                                          bool include_tree_stats) {
  auto sorted_child_info = player_->tree().CalculateRankedMoveInfo();
  auto* root = player_->root();

  std::ostringstream moveinfo;

  Coord c = sorted_child_info[0].c;
  const auto child_it = root->children.find(c);
  if (child_it == root->children.end() or root->child_N(c) == 0) {
   // moveinfo << "info";
    return;
  }

  // TODO(tommadams): Make the number of child variations sent back
  // configurable.

  for (int i = 0; i < 20; ++i) {
    Coord c = sorted_child_info[i].c;
    const auto child_it = root->children.find(c);
    if (child_it == root->children.end() || root->child_N(c) == 0) {
      break;
    }
      
    int winrate;
    if (player_->root()->position.to_play() == Color::kBlack){
    winrate = floor(((root->child_Q(c)+1) / 2)*10000);
    }
    else{
    winrate = 10000 - floor(((root->child_Q(c)+1) / 2)*10000);
    }
    moveinfo << "info move " << c.ToGtp();
    moveinfo << " visits " << root->child_N(c);
    moveinfo << " winrate " << winrate;
    moveinfo << " order " << i;
    moveinfo << " pv ";
    moveinfo << c.ToGtp();

    const auto* node = child_it->second.get();
    for (const auto c : node->GetMostVisitedPath()) {
       moveinfo << " ";
       moveinfo << c.ToGtp();
      }
    moveinfo << " ";
    }

    std::cout << moveinfo.str()<< "\n";
  }



void MiniguiGtpClient::ReportRootPosition() {
  const auto* root = player_->root();
  const auto& position = root->position;

  std::ostringstream oss;
  for (const auto& stone : position.stones()) {
    char ch;
    if (stone.color() == Color::kBlack) {
      ch = 'X';
    } else if (stone.color() == Color::kWhite) {
      ch = 'O';
    } else {
      ch = '.';
    }
    oss << ch;
  }

  nlohmann::json j = {
      {"id", variation_tree_->current_node()->id},
      {"toPlay", position.to_play() == Color::kBlack ? "B" : "W"},
      {"moveNum", position.n()},
      {"stones", oss.str()},
      {"gameOver", root->game_over()},
  };

  const auto& captures = position.num_captures();
  if (captures[0] != 0 || captures[1] != 0) {
    j["caps"].push_back(captures[0]);
    j["caps"].push_back(captures[1]);
  }
  if (root->parent != nullptr) {
    j["parentId"] = variation_tree_->current_node()->parent->id;
    if (root->N() > 0) {
      // Only send Q if the node has been read at least once.
      j["q"] = GetBestMoveQ(root);
    }
  }
  if (root->move != Coord::kInvalid) {
    j["move"] = root->move.ToGtp();
  }
  const auto& comment = variation_tree_->current_node()->comment;
  if (!comment.empty()) {
    j["comment"] = comment;
  }

  MG_LOG(INFO) << "mg-position: " << j.dump();
}

void MiniguiGtpClient::RefreshPendingWinRateEvals() {
  // Build a new list of nodes that require win rate evaluation.
  // First, traverse to the leaf node of the current position's main line.
  auto* node = variation_tree_->current_node();
  while (!node->children.empty()) {
    node = node->children[0];
  }

  std::vector<VariationTree::Node*> variation;
  while (node != nullptr) {
    variation.push_back(node);
    node = node->parent;
  }
  std::reverse(variation.begin(), variation.end());

  win_rate_evaluator_->SetCurrentVariation(std::move(variation));
}

void MiniguiGtpClient::TreeSearchCb(
  const std::vector<const MctsNode*>& leaves) {
  
  if (!leaves.empty() && report_search_interval_ != absl::ZeroDuration()) {
    auto now = absl::Now();
    if (now - last_report_time_ > report_search_interval_) {
      last_report_time_ = now;
      ReportSearchStatus(leaves.back(), false);
    }
  }
}

MiniguiGtpClient::VariationTree::Node::Node(Node* parent, Coord move)
    : parent(parent),
      move(move),
      id(parent == nullptr ? "root" : absl::StrFormat("%p", this)),
      n(parent != nullptr ? parent->n + 1 : 0) {}

std::vector<Coord> MiniguiGtpClient::VariationTree::Node::GetVariation() const {
  std::vector<Coord> variation;
  for (auto* node = this; node->parent != nullptr; node = node->parent) {
    variation.push_back(node->move);
  }
  std::reverse(variation.begin(), variation.end());
  return variation;
}

MiniguiGtpClient::VariationTree::VariationTree() {
  auto root = absl::make_unique<Node>(nullptr, Coord::kInvalid);
  current_node_ = root.get();
  id_map_.emplace(root->id, std::move(root));
}

void MiniguiGtpClient::VariationTree::PlayMove(Coord c) {
  // Check if the current node already has a child with the given move.
  for (auto* child : current_node_->children) {
    if (child->move == c) {
      current_node_ = child;
      return;
    }
  }

  // Create a new child.
  auto child = absl::make_unique<Node>(current_node_, c);
  current_node_->children.push_back(child.get());
  current_node_ = current_node_->children.back();
  id_map_.emplace(current_node_->id, std::move(child));
}

void MiniguiGtpClient::VariationTree::GoToParent() {
  MG_CHECK(current_node_->parent != nullptr);
  current_node_ = current_node_->parent;
}

void MiniguiGtpClient::VariationTree::GoToStart() {
  while (current_node_->parent != nullptr) {
    current_node_ = current_node_->parent;
  }
}

bool MiniguiGtpClient::VariationTree::SelectNode(const std::string& id) {
  auto it = id_map_.find(id);
  if (it == id_map_.end()) {
    return false;
  }
  current_node_ = it->second.get();
  return true;
}

MiniguiGtpClient::WinRateEvaluator::WinRateEvaluator(
    int num_workers, int num_eval_reads, const std::string& device,
    std::shared_ptr<ThreadSafeInferenceCache> inference_cache,
    const std::string& model_path, const Game::Options& game_options,
    const MctsPlayer::Options& player_options)
    : num_eval_reads_(num_eval_reads) {
  MG_CHECK(inference_cache != nullptr);

  batcher_ = absl::make_unique<BatchingModelFactory>(device, 2);
  for (int i = 0; i < num_workers; ++i) {
    auto game = absl::make_unique<Game>("b", "w", game_options);
    auto player = absl::make_unique<MctsPlayer>(batcher_->NewModel(model_path),
                                                inference_cache, game.get(),
                                                player_options);
    workers_.push_back(absl::make_unique<Worker>(
        std::move(game), std::move(player), &eval_queue_));
    workers_.back()->Start();
  }
}

MiniguiGtpClient::WinRateEvaluator::~WinRateEvaluator() = default;

void MiniguiGtpClient::WinRateEvaluator::SetNumEvalReads(int num_eval_reads) {
  num_eval_reads_ = num_eval_reads;
  UpdateNodesToEval();
}

void MiniguiGtpClient::WinRateEvaluator::SetCurrentVariation(
    std::vector<VariationTree::Node*> nodes) {
  variation_ = std::move(nodes);
  UpdateNodesToEval();
}

void MiniguiGtpClient::WinRateEvaluator::EvalNodes() {
  auto num_inferences = std::min(workers_.size(), to_eval_.size());
  if (num_inferences == 0) {
    return;
  }

  // Inform each worker how many inferences we want to run in parallel.
  // This allows the batcher to know how many inferences to expect.
  for (size_t i = 0; i < num_inferences; ++i) {
    workers_[i]->Prepare();
  }

  // Run win rate evaluation in parallel.
  for (size_t i = 0; i < num_inferences; ++i) {
    workers_[i]->EvalAsync(to_eval_.front());
    to_eval_.pop_front();
  }

  // Wait for the workers to finish.
  for (size_t i = 0; i < num_inferences; ++i) {
    auto* node = eval_queue_.Pop();
    // auto* node = workers_[i]->Wait();
    if (node->num_eval_reads < num_eval_reads_) {
      to_eval_.push_back(node);
    }
  }
}

void MiniguiGtpClient::WinRateEvaluator::UpdateNodesToEval() {
  to_eval_.clear();
  for (auto* node : variation_) {
    if (node->num_eval_reads < num_eval_reads_) {
      to_eval_.push_back(node);
    }
  }

  // Sort the nodes for eval by number of eval reads, breaking ties by the move
  // number.
  std::sort(to_eval_.begin(), to_eval_.end(),
            [](VariationTree::Node* a, VariationTree::Node* b) {
              if (a->num_eval_reads != b->num_eval_reads) {
                return a->num_eval_reads < b->num_eval_reads;
              }
              return a->n < b->n;
            });
}

MiniguiGtpClient::WinRateEvaluator::Worker::Worker(
    std::unique_ptr<Game> game, std::unique_ptr<MctsPlayer> player,
    ThreadSafeQueue<VariationTree::Node*>* eval_queue)
    : game_(std::move(game)),
      player_(std::move(player)),
      eval_queue_(eval_queue) {}

MiniguiGtpClient::WinRateEvaluator::Worker::~Worker() {
  absl::MutexLock lock(&mutex_);
  MG_CHECK(!pending_.has_value());
  pending_ = nullptr;
  Join();
}

void MiniguiGtpClient::WinRateEvaluator::Worker::Prepare() {
  absl::MutexLock lock(&mutex_);
  BatchingModelFactory::StartGame(player_->model(), player_->model());
}

void MiniguiGtpClient::WinRateEvaluator::Worker::EvalAsync(
    VariationTree::Node* node) {
  absl::MutexLock lock(&mutex_);
  MG_CHECK(!pending_.has_value());
  pending_ = node;
}

void MiniguiGtpClient::WinRateEvaluator::Worker::Run() {
  for (;;) {
    absl::MutexLock lock(&mutex_);
    mutex_.Await(absl::Condition(this, &Worker::has_pending_value));
    auto* node = *pending_;
    pending_.reset();
    if (node == nullptr) {
      break;
    }

    player_->NewGame();
    for (auto c : node->GetVariation()) {
      MG_CHECK(player_->PlayMove(c));
    }
    player_->TreeSearch(player_->options().virtual_losses, 1024, player_->stop_tree_search_);
    BatchingModelFactory::EndGame(player_->model(), player_->model());

    nlohmann::json j = {
        {"id", node->id},
        {"n", player_->root()->N()},
        {"q", GetBestMoveQ(player_->root())},
    };
   // MG_LOG(INFO) << "mg-update:" << j.dump();

    node->num_eval_reads = player_->root()->N();
    eval_queue_->Push(node);
  }
}

}  // namespace minigo
