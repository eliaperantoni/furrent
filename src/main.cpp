#include <fstream>
#include <mutex>
#include <vector>

#include "bencode/bencode_parser.hpp"
#include "download/downloader.hpp"
#include "download/lender_pool.hpp"
#include "log/logger.hpp"
#include "peer.hpp"
#include "torrent.hpp"

using namespace fur::bencode;
using namespace fur::peer;
using namespace fur::download::lender_pool;
using namespace fur::download::downloader;

void work() {}

int main(int argc, char* argv[]) {
  fur::log::initialize_custom_logger();
  auto logger = spdlog::get("custom");
  logger->set_level(spdlog::level::debug);

  if (argc != 2) {
    logger->error("usage: furrent <path>");
    return 1;
  }

  // Read torrent file to string
  std::ifstream f(argv[1]);
  std::stringstream buf;
  buf << f.rdbuf();
  std::string content = buf.str();

  // Parse bencode
  BencodeParser parser;
  auto ben_tree = parser.decode(content);

  // Parse TorrentFile
  TorrentFile torrent(*ben_tree);

  std::vector<Task> tasks;
  std::mutex tasks_mx;

  for (int i = 0; i < static_cast<int>(torrent.piece_hashes.size()); i++) {
    tasks.push_back({i});
  }

  LenderPool<Downloader> downloaders;

  auto refresher = std::thread([&]() {
    while (true) {
      logger->info("Drain start");
      downloaders.drain();
      logger->info("Drain done");

      for (auto peer : announce(torrent).peers) {
        Downloader down(torrent, peer);
        if (down.ensure_connected().valid()) {
          downloaders.put(std::move(down));
        }
      }

      std::this_thread::sleep_for(std::chrono::minutes(2));
    }
  });

  const int N_THREADS = 4;

  int done = 0;
  std::mutex done_mx;

  std::vector<std::thread> threads;
  for (int i = 0; i < N_THREADS; i++) {
    threads.emplace_back([&]() {
      while (true) {
        Task task{};
        {
          std::unique_lock lock(tasks_mx);
          if (tasks.empty()) return;
          task = tasks.front();
          tasks.erase(tasks.begin());
        }

        auto down = downloaders.get();

        auto maybe_result = down->try_download(task);
        if (!maybe_result.valid()) {
          std::unique_lock lock(tasks_mx);
          tasks.push_back(task);
        } else {
          std::unique_lock lock(done_mx);
          done++;
          logger->info("Remaining {}: {:.2f}%",
                       static_cast<int>(torrent.piece_hashes.size()) - done,
                       100 * static_cast<float>(done) /
                           static_cast<float>(torrent.piece_hashes.size()));
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  logger->info("Done");

  refresher.join();

  return 0;
}
